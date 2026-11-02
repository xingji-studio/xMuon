#include "browser_image_decode.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>
#include <math.h>
#include <stdlib.h>

#include "third_party/libwebp/src/webp/decode.h"

#define NANOSVG_CPLUSPLUS

namespace
{

constexpr std::size_t kMaxEncodedImageBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxDecodedPixels = 4096 * 4096;
constexpr std::size_t kMaxDecoderBytes = 128 * 1024 * 1024;

struct ImageDecodeFailure
{
    const char *message;
};

struct ImageAllocation;

// Decoding is owned by the browser's main thread. Keep temporary codec storage
// in one scope so even a codec's interrupted realloc/error path cannot leak it.
struct ImageDecodeScope
{
    ImageDecodeScope *previous;
    ImageAllocation *head = nullptr;
    std::size_t used = 0;
    bool invalid_number = false;

    ImageDecodeScope();
    ~ImageDecodeScope();
};

struct alignas(std::max_align_t) ImageAllocation
{
    ImageDecodeScope *owner;
    ImageAllocation *previous;
    ImageAllocation *next;
    std::size_t size;
};

static ImageDecodeScope *g_image_decode_scope = nullptr;

ImageDecodeScope::ImageDecodeScope() : previous(g_image_decode_scope)
{
    g_image_decode_scope = this;
}

ImageDecodeScope::~ImageDecodeScope()
{
    while (head != nullptr)
    {
        ImageAllocation *next = head->next;
        std::free(head);
        head = next;
    }
    g_image_decode_scope = previous;
}

static void *image_codec_malloc(std::size_t bytes)
{
    ImageDecodeScope *scope = g_image_decode_scope;
    if (bytes > kMaxDecoderBytes - sizeof(ImageAllocation))
    {
        throw ImageDecodeFailure{"图片解码临时内存超过限制"};
    }
    const std::size_t charged = sizeof(ImageAllocation) + bytes;
    if (scope != nullptr && charged > kMaxDecoderBytes - scope->used)
    {
        throw ImageDecodeFailure{"图片解码临时内存超过限制"};
    }
    auto *allocation = static_cast<ImageAllocation *>(std::malloc(charged));
    if (allocation == nullptr) throw std::bad_alloc();
    allocation->owner = scope;
    allocation->previous = nullptr;
    allocation->next = scope == nullptr ? nullptr : scope->head;
    allocation->size = bytes;
    if (scope != nullptr)
    {
        if (scope->head != nullptr) scope->head->previous = allocation;
        scope->head = allocation;
        scope->used += charged;
    }
    return allocation + 1;
}

static void image_codec_free(void *pointer)
{
    if (pointer == nullptr) return;
    auto *allocation = static_cast<ImageAllocation *>(pointer) - 1;
    ImageDecodeScope *scope = allocation->owner;
    if (scope != nullptr)
    {
        if (allocation->previous != nullptr)
            allocation->previous->next = allocation->next;
        else
            scope->head = allocation->next;
        if (allocation->next != nullptr) allocation->next->previous = allocation->previous;
        scope->used -= sizeof(ImageAllocation) + allocation->size;
    }
    std::free(allocation);
}

static void *image_codec_realloc(void *pointer, std::size_t bytes)
{
    if (pointer == nullptr) return image_codec_malloc(bytes);
    if (bytes == 0)
    {
        image_codec_free(pointer);
        return nullptr;
    }
    auto *allocation = static_cast<ImageAllocation *>(pointer) - 1;
    void *replacement = image_codec_malloc(bytes);
    std::memcpy(replacement, pointer, std::min(bytes, allocation->size));
    image_codec_free(pointer);
    return replacement;
}

static bool image_finite(float value)
{
    // Use the compiler primitive: the hosted math compatibility layer is partial.
    return __builtin_isfinite(value);
}

static void image_invalid_number()
{
    if (g_image_decode_scope != nullptr) g_image_decode_scope->invalid_number = true;
}

static bool valid_image_size(int width, int height)
{
    return width > 0 && height > 0 &&
           static_cast<std::size_t>(width) <= kMaxDecodedPixels / static_cast<std::size_t>(height);
}

} // namespace

static float nanosvg_fabsf(float value)
{
    return value < 0.0f ? -value : value;
}

extern "C" float floorf(float value) noexcept
{
    if (!image_finite(value))
    {
        image_invalid_number();
        return 0.0f;
    }
    // Every binary32 value beyond this point is already integral. Avoid an
    // out-of-range float-to-long conversion on hostile SVG coordinates.
    if (nanosvg_fabsf(value) >= 8388608.0f) return value;
    const long truncated = static_cast<long>(value);
    if (value < 0.0f && static_cast<float>(truncated) != value) {
        return static_cast<float>(truncated - 1);
    }
    return static_cast<float>(truncated);
}

extern "C" float ceilf(float value) noexcept
{
    if (!image_finite(value))
    {
        image_invalid_number();
        return 0.0f;
    }
    if (nanosvg_fabsf(value) >= 8388608.0f) return value;
    const long truncated = static_cast<long>(value);
    if (value > 0.0f && static_cast<float>(truncated) != value) {
        return static_cast<float>(truncated + 1);
    }
    return static_cast<float>(truncated);
}

static float nanosvg_sqrtf(float value)
{
    if (value <= 0.0f) {
        return 0.0f;
    }

    float estimate = value > 1.0f ? value : 1.0f;
    for (int i = 0; i < 8; ++i) {
        estimate = 0.5f * (estimate + value / estimate);
    }
    return estimate;
}

static float nanosvg_wrap_radians(float value)
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kTwoPi = 6.28318530717958647692;
    if (!image_finite(value))
    {
        image_invalid_number();
        return 0.0f;
    }

    double remainder = value < 0.0f ? -static_cast<double>(value) : static_cast<double>(value);
    double divisor = kTwoPi;
    // Binary scaling takes at most 126 steps for any finite binary32 input.
    // Unlike repeated subtraction (or the runtime's integer-based fmod), it
    // always makes progress even when value is so large that value - 2*pi == value.
    while (divisor <= remainder / 2.0) divisor *= 2.0;
    while (divisor >= kTwoPi)
    {
        if (remainder >= divisor) remainder -= divisor;
        divisor *= 0.5;
    }
    if (remainder > kPi) remainder -= kTwoPi;
    return static_cast<float>(value < 0.0f ? -remainder : remainder);
}

extern "C" float sinf(float value) noexcept
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kHalfPi = 1.57079632679489661923f;

    value = nanosvg_wrap_radians(value);
    if (value > kHalfPi) {
        value = kPi - value;
    } else if (value < -kHalfPi) {
        value = -kPi - value;
    }

    const float x2 = value * value;
    return value * (1.0f - x2 * (1.0f / 6.0f) + x2 * x2 * (1.0f / 120.0f) -
                    x2 * x2 * x2 * (1.0f / 5040.0f));
}

extern "C" float cosf(float value) noexcept
{
    constexpr float kHalfPi = 1.57079632679489661923f;
    return sinf(kHalfPi - nanosvg_wrap_radians(value));
}

extern "C" float tanf(float value) noexcept
{
    const float cosine = cosf(value);
    if (nanosvg_fabsf(cosine) < 1e-5f) {
        return value >= 0.0f ? 1e6f : -1e6f;
    }
    return sinf(value) / cosine;
}

static float nanosvg_atanf(float value)
{
    constexpr float kQuarterPi = 0.78539816339744830962f;
    if (value > 1.0f) {
        return 1.57079632679489661923f - nanosvg_atanf(1.0f / value);
    }
    if (value < -1.0f) {
        return -1.57079632679489661923f - nanosvg_atanf(1.0f / value);
    }

    const float abs_value = nanosvg_fabsf(value);
    return value * (kQuarterPi + 0.273f * (1.0f - abs_value));
}

extern "C" float atan2f(float y, float x) noexcept
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kHalfPi = 1.57079632679489661923f;

    if (!image_finite(x) || !image_finite(y))
    {
        image_invalid_number();
        return 0.0f;
    }

    if (x > 0.0f) {
        return nanosvg_atanf(y / x);
    }
    if (x < 0.0f) {
        return y >= 0.0f ? nanosvg_atanf(y / x) + kPi : nanosvg_atanf(y / x) - kPi;
    }
    if (y > 0.0f) {
        return kHalfPi;
    }
    if (y < 0.0f) {
        return -kHalfPi;
    }
    return 0.0f;
}

extern "C" float acosf(float value) noexcept
{
    if (!image_finite(value))
    {
        image_invalid_number();
        return 0.0f;
    }
    if (value <= -1.0f) {
        return 3.14159265358979323846f;
    }
    if (value >= 1.0f) {
        return 0.0f;
    }
    const float y = nanosvg_sqrtf(1.0f - value * value);
    return atan2f(y, value);
}

static void nanosvg_swap_bytes(unsigned char *lhs, unsigned char *rhs, std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i) {
        const unsigned char tmp = lhs[i];
        lhs[i] = rhs[i];
        rhs[i] = tmp;
    }
}

extern "C" __attribute__((weak)) void qsort(void *base,
                                            std::size_t count,
                                            std::size_t size,
                                            int (*compare)(const void *, const void *)) noexcept
{
    if (size == 0 || count < 2) {
        return;
    }

    unsigned char *bytes = static_cast<unsigned char *>(base);
    for (std::size_t i = 1; i < count; ++i) {
        std::size_t j = i;
        while (j > 0) {
            unsigned char *current = bytes + j * size;
            unsigned char *previous = current - size;
            if (compare(previous, current) <= 0) {
                break;
            }
            nanosvg_swap_bytes(previous, current, size);
            --j;
        }
    }
}

static long long nanosvg_strtoll(const char *text, char **endptr, int base)
{
    // NanoSVG only requests decimal integers. Its integer and exponent parts
    // originate in untrusted input, so do not rely on overflowing libc shims.
    const char *cursor = text;
    while (*cursor != '\0' && std::isspace(static_cast<unsigned char>(*cursor)) != 0) ++cursor;
    const bool negative = *cursor == '-';
    if (*cursor == '-' || *cursor == '+') ++cursor;
    const char *first = cursor;
    const unsigned long long limit = static_cast<unsigned long long>(LLONG_MAX) + (negative ? 1ULL : 0ULL);
    unsigned long long value = 0;
    bool overflow = base != 10;
    while (*cursor >= '0' && *cursor <= '9')
    {
        const unsigned int digit = static_cast<unsigned int>(*cursor++ - '0');
        if (value > (limit - digit) / 10ULL)
            overflow = true;
        else
            value = value * 10ULL + digit;
    }
    if (endptr != nullptr) *endptr = const_cast<char *>(cursor == first ? text : cursor);
    if (overflow)
    {
        image_invalid_number();
        return 0;
    }
    if (negative && value == static_cast<unsigned long long>(LLONG_MAX) + 1ULL) return LLONG_MIN;
    return negative ? -static_cast<long long>(value) : static_cast<long long>(value);
}

static long nanosvg_strtol(const char *text, char **endptr, int base)
{
    return static_cast<long>(nanosvg_strtoll(text, endptr, base));
}

static int parse_hex_digits_exact(const char *text, int digits, unsigned int *out_value)
{
    if (text == nullptr || out_value == nullptr) {
        return 0;
    }

    unsigned int value = 0;
    for (int i = 0; i < digits; ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        unsigned int digit = 0;
        if (ch >= '0' && ch <= '9') {
            digit = static_cast<unsigned int>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = static_cast<unsigned int>(ch - 'a' + 10);
        } else if (ch >= 'A' && ch <= 'F') {
            digit = static_cast<unsigned int>(ch - 'A' + 10);
        } else {
            return 0;
        }
        value = (value << 4U) | digit;
    }

    *out_value = value;
    return 1;
}

static const char *skip_spaces(const char *text)
{
    while (text != nullptr && *text != '\0' && std::isspace(static_cast<unsigned char>(*text)) != 0) {
        ++text;
    }
    return text;
}

static int parse_decimal_component(const char **cursor, unsigned int *value)
{
    const char *p = skip_spaces(*cursor);
    if (p == nullptr || *p < '0' || *p > '9') {
        return 0;
    }

    unsigned int parsed = 0;
    while (*p >= '0' && *p <= '9') {
        parsed = parsed * 10U + static_cast<unsigned int>(*p - '0');
        ++p;
    }
    *cursor = skip_spaces(p);
    *value = parsed;
    return 1;
}

static int nanosvg_sscanf(const char *text, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int matched = 0;

    if (std::strcmp(format, "#%2x%2x%2x") == 0) {
        unsigned int *a = va_arg(args, unsigned int *);
        unsigned int *b = va_arg(args, unsigned int *);
        unsigned int *c = va_arg(args, unsigned int *);
        if (text != nullptr && text[0] == '#' &&
            parse_hex_digits_exact(text + 1, 2, a) &&
            parse_hex_digits_exact(text + 3, 2, b) &&
            parse_hex_digits_exact(text + 5, 2, c)) {
            matched = 3;
        }
    } else if (std::strcmp(format, "#%1x%1x%1x") == 0) {
        unsigned int *a = va_arg(args, unsigned int *);
        unsigned int *b = va_arg(args, unsigned int *);
        unsigned int *c = va_arg(args, unsigned int *);
        if (text != nullptr && text[0] == '#' &&
            parse_hex_digits_exact(text + 1, 1, a) &&
            parse_hex_digits_exact(text + 2, 1, b) &&
            parse_hex_digits_exact(text + 3, 1, c)) {
            matched = 3;
        }
    } else if (std::strcmp(format, "rgb(%u, %u, %u)") == 0) {
        unsigned int *a = va_arg(args, unsigned int *);
        unsigned int *b = va_arg(args, unsigned int *);
        unsigned int *c = va_arg(args, unsigned int *);
        const char *p = text;
        if (p != nullptr && std::strncmp(p, "rgb(", 4) == 0) {
            p += 4;
            if (parse_decimal_component(&p, a) && *p == ',') {
                ++p;
                if (parse_decimal_component(&p, b) && *p == ',') {
                    ++p;
                    if (parse_decimal_component(&p, c) && *p == ')') {
                        matched = 3;
                    }
                }
            }
        }
    }

    va_end(args);
    return matched;
}

#define sscanf nanosvg_sscanf
#define strtoll nanosvg_strtoll
#define strtol nanosvg_strtol
#define malloc image_codec_malloc
#define realloc image_codec_realloc
#define free image_codec_free
#define NANOSVG_IMPLEMENTATION
#include "third_party/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "third_party/nanosvg/nanosvgrast.h"
#undef sscanf
#undef strtoll
#undef strtol
#undef malloc
#undef realloc
#undef free

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_STDIO
#define STBI_MALLOC image_codec_malloc
#define STBI_REALLOC image_codec_realloc
#define STBI_FREE image_codec_free
#include "third_party/stb/stbi.h"

namespace
{

static std::string lower_copy(const std::string &value)
{
    std::string lowered = value;
    for (char &ch : lowered) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return lowered;
}

static bool looks_like_svg(const std::string &encoded, const std::string &content_type)
{
    const std::string lowered_type = lower_copy(content_type);
    if (lowered_type.find("image/svg+xml") != std::string::npos) {
        return true;
    }

    std::size_t start = 0;
    if (encoded.size() >= 3 &&
        static_cast<unsigned char>(encoded[0]) == 0xEF &&
        static_cast<unsigned char>(encoded[1]) == 0xBB &&
        static_cast<unsigned char>(encoded[2]) == 0xBF) {
        start = 3;
    }
    while (start < encoded.size() && std::isspace(static_cast<unsigned char>(encoded[start])) != 0) {
        ++start;
    }

    return encoded.compare(start, 4, "<svg") == 0 ||
           encoded.compare(start, 5, "<?xml") == 0;
}

static bool looks_like_webp(const std::string &encoded)
{
    return encoded.size() >= 12 &&
           std::memcmp(encoded.data(), "RIFF", 4) == 0 &&
           std::memcmp(encoded.data() + 8, "WEBP", 4) == 0;
}

static bool decode_webp(const std::string &encoded,
                        BrowserDecodedImage &decoded,
                        std::string &error)
{
    int width = 0;
    int height = 0;
    if (WebPGetInfo(reinterpret_cast<const std::uint8_t *>(encoded.data()),
                    encoded.size(),
                    &width,
                    &height) == 0) {
        error = "WebP 头无效";
        return false;
    }

    if (!valid_image_size(width, height)) {
        error = "解码后的图片超过限制";
        return false;
    }
    const std::size_t pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    decoded.width = width;
    decoded.height = height;
    decoded.pixels.resize(pixel_count);
    std::uint8_t *out = reinterpret_cast<std::uint8_t *>(decoded.pixels.data());
    if (WebPDecodeRGBAInto(reinterpret_cast<const std::uint8_t *>(encoded.data()),
                           encoded.size(),
                           out,
                           pixel_count * sizeof(XCOLORA),
                           width * static_cast<int>(sizeof(XCOLORA))) == nullptr) {
        decoded = BrowserDecodedImage();
        error = "WebP 解码失败";
        return false;
    }
    return true;
}

static bool decode_svg(const std::string &encoded,
                       BrowserDecodedImage &decoded,
                       std::string &error)
{
    std::vector<char> source(encoded.begin(), encoded.end());
    source.push_back('\0');

    std::unique_ptr<NSVGimage, decltype(&nsvgDelete)> image(nsvgParse(source.data(), "px", 96.0f), nsvgDelete);
    if (image == nullptr) {
        error = "SVG 解析失败";
        return false;
    }

    if (g_image_decode_scope->invalid_number || !image_finite(image->width) || !image_finite(image->height) ||
        image->width <= 0.0f || image->height <= 0.0f ||
        image->width > static_cast<float>(kMaxDecodedPixels) ||
        image->height > static_cast<float>(kMaxDecodedPixels))
    {
        error = "解码后的图片超过限制";
        return false;
    }
    const int width = static_cast<int>(image->width + 0.5f);
    const int height = static_cast<int>(image->height + 0.5f);
    if (!valid_image_size(width, height))
    {
        error = "解码后的图片超过限制";
        return false;
    }
    const std::size_t pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    std::unique_ptr<NSVGrasterizer, decltype(&nsvgDeleteRasterizer)> rasterizer(nsvgCreateRasterizer(),
                                                                           nsvgDeleteRasterizer);
    if (rasterizer == nullptr) {
        error = "SVG 光栅化器初始化失败";
        return false;
    }

    decoded.width = width;
    decoded.height = height;
    decoded.pixels.resize(pixel_count);
    std::memset(decoded.pixels.data(), 0, pixel_count * sizeof(XCOLORA));
    nsvgRasterize(rasterizer.get(),
                  image.get(),
                  0.0f,
                  0.0f,
                  1.0f,
                  reinterpret_cast<unsigned char *>(decoded.pixels.data()),
                  width,
                  height,
                  width * static_cast<int>(sizeof(XCOLORA)));

    if (g_image_decode_scope->invalid_number)
    {
        decoded = BrowserDecodedImage();
        error = "SVG 包含无效数值";
        return false;
    }
    return true;
}

} // namespace

bool browser_decode_image(const std::string &encoded,
                          const std::string &content_type,
                          BrowserDecodedImage &decoded,
                          std::string &error)
{
    decoded = BrowserDecodedImage();
    error.clear();

    try
    {
        ImageDecodeScope scope;

        if (encoded.empty()) {
            error = "图片内容为空";
            return false;
        }
        if (encoded.size() > kMaxEncodedImageBytes) {
            error = "图片过大";
            return false;
        }

        if (looks_like_svg(encoded, content_type)) {
            return decode_svg(encoded, decoded, error);
        }

        if (looks_like_webp(encoded)) {
            return decode_webp(encoded, decoded, error);
        }

        int width = 0;
        int height = 0;
        int channels = 0;
        if (stbi_info_from_memory(reinterpret_cast<const stbi_uc *>(encoded.data()), static_cast<int>(encoded.size()),
                                 &width, &height, &channels) == 0)
        {
            error = "图片头无效";
            return false;
        }
        if (!valid_image_size(width, height))
        {
            error = "解码后的图片超过限制";
            return false;
        }

        std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> data(
            stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(encoded.data()), static_cast<int>(encoded.size()),
                                  &width, &height, &channels, 4),
            stbi_image_free);
        if (data == nullptr) {
            error = "图片解码失败";
            return false;
        }

        if (!valid_image_size(width, height)) {
            error = "解码后的图片超过限制";
            return false;
        }
        const std::size_t pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

        decoded.width = width;
        decoded.height = height;
        decoded.pixels.resize(pixel_count);
        std::memcpy(decoded.pixels.data(), data.get(), pixel_count * sizeof(XCOLORA));
        return true;
    }
    catch (const ImageDecodeFailure &failure)
    {
        decoded = BrowserDecodedImage();
        error = failure.message;
        return false;
    }
    catch (const std::bad_alloc &)
    {
        decoded = BrowserDecodedImage();
        error = "图片解码内存不足";
        return false;
    }
}
