#include "browser_content_encoding.h"
#include "../xarchive/deflate.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <new>

namespace
{

constexpr std::array<std::uint32_t, 256> make_crc_table()
{
    std::array<std::uint32_t, 256> result {};
    for (unsigned index = 0; index < result.size(); ++index)
    {
        std::uint32_t value = index;
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((value & 1) ? 0xedb88320U : 0U);
        result[index] = value;
    }
    return result;
}

constexpr auto crc_table = make_crc_table();

std::uint32_t crc_update(std::uint32_t value, const void *data, std::size_t size)
{
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (std::size_t index = 0; index < size; ++index)
        value = crc_table[(value ^ bytes[index]) & 255] ^ (value >> 8);
    return value;
}

unsigned little16(const unsigned char *bytes)
{
    return bytes[0] | (static_cast<unsigned>(bytes[1]) << 8);
}

std::uint32_t little32(const unsigned char *bytes)
{
    return bytes[0] | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::string normalized_encoding(std::string value)
{
    auto space = [](char ch) { return ch == ' ' || ch == '\t'; };
    while (!value.empty() && space(value.back())) value.pop_back();
    std::size_t start = 0;
    while (start < value.size() && space(value[start])) ++start;
    value.erase(0, start);
    for (char &ch : value) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return value;
}

bool reject(std::string &error, const char *message)
{
    error = message;
    return false;
}

struct Input
{
    const char *data;
    std::size_t size, position = 0;
};

ssize_t read_bytes(void *context, void *buffer, std::size_t capacity)
{
    auto &input = *static_cast<Input *>(context);
    const std::size_t amount = std::min(capacity, input.size - input.position);
    if (amount) std::memcpy(buffer, input.data + input.position, amount);
    input.position += amount;
    return static_cast<ssize_t>(amount);
}

struct Output
{
    std::string data;
    std::size_t limit;
    std::uint32_t crc = 0xffffffffU;
};

int write_bytes(void *context, const void *buffer, std::size_t size)
{
    auto &output = *static_cast<Output *>(context);
    if (size > output.limit - output.data.size()) return -1;
    output.data.append(static_cast<const char *>(buffer), size);
    output.crc = crc_update(output.crc, buffer, size);
    return 0;
}

bool decode_gzip(std::string &body, std::size_t limit, std::string &error)
{
    if (body.size() < 18) return reject(error, "Truncated gzip representation");
    const auto *bytes = reinterpret_cast<const unsigned char *>(body.data());
    if (bytes[0] != 0x1f || bytes[1] != 0x8b || bytes[2] != 8 || (bytes[3] & 0xe0))
        return reject(error, "Invalid gzip header");

    const std::size_t trailer = body.size() - 8;
    std::size_t position = 10;
    const unsigned flags = bytes[3];
    if (flags & 4)
    {
        if (trailer - position < 2) return reject(error, "Truncated gzip extra length");
        const unsigned extra = little16(bytes + position);
        position += 2;
        if (extra > trailer - position) return reject(error, "Truncated gzip extra field");
        position += extra;
    }
    // The optional filename and comment are NUL-terminated byte strings, not
    // filenames to open. Every scan stays before the fixed eight-byte trailer.
    for (const unsigned field : {8U, 16U})
    {
        if (!(flags & field)) continue;
        while (position < trailer && bytes[position] != 0) ++position;
        if (position == trailer) return reject(error, "Truncated gzip optional header");
        ++position;
    }
    if (flags & 2)
    {
        if (trailer - position < 2) return reject(error, "Truncated gzip header checksum");
        const unsigned expected = little16(bytes + position);
        if (((crc_update(0xffffffffU, body.data(), position) ^ 0xffffffffU) & 65535) != expected)
            return reject(error, "Gzip header checksum mismatch");
        position += 2;
    }
    if (position >= trailer) return reject(error, "Missing gzip deflate payload");

    const std::uint32_t expected_crc = little32(bytes + trailer);
    const std::uint32_t expected_size = little32(bytes + trailer + 4);
    if (expected_size > limit) return reject(error, "Decoded response exceeds browser limit");
    Input input {body.data() + position, trailer - position};
    Output output {{}, expected_size};
    output.data.reserve(expected_size);
    // ISIZE is only a claim. The raw codec independently bounds every output
    // byte and requires the complete input to end at the final deflate block.
    if (xa_deflate_decompress_stream(read_bytes, &input, input.size, write_bytes, &output, expected_size) != 0 ||
        output.data.size() != expected_size)
        return reject(error, "Invalid or truncated gzip deflate payload");
    if ((output.crc ^ 0xffffffffU) != expected_crc) return reject(error, "Gzip payload checksum mismatch");
    body.swap(output.data);
    return true;
}

} // namespace

bool browser_decode_content(std::string &body, const std::string &encoding, std::size_t limit, std::string &error)
{
    error.clear();
    const std::string coding = normalized_encoding(encoding);
    if (coding.empty() || coding == "identity")
        return body.size() <= limit || reject(error, "Response exceeds browser limit");
    if (coding != "gzip" && coding != "x-gzip") return reject(error, "Unsupported HTTP content encoding");
    try
    {
        return decode_gzip(body, limit, error);
    }
    catch (const std::bad_alloc &)
    {
        return reject(error, "Insufficient memory to decode response");
    }
}
