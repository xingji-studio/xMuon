#pragma once

#include <algorithm>
#include <string>

namespace browser_url_detail
{

struct Parts
{
    std::string scheme, authority, path, query, fragment;
    bool has_authority = false, has_query = false, has_fragment = false;
};

inline bool scheme_letter(char value)
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

inline bool parse(const std::string &text, Parts &parts)
{
    for (unsigned char ch : text)
        if (ch <= 0x20 || ch == 0x7f || ch == '\\') return false;

    std::size_t cursor = 0;
    const auto colon = text.find(':');
    const auto first_separator = text.find_first_of("/?#");
    if (colon != std::string::npos && (first_separator == std::string::npos || colon < first_separator))
    {
        if (colon == 0 || !scheme_letter(text[0])) return false;
        for (std::size_t i = 1; i < colon; ++i)
        {
            const char ch = text[i];
            if (!scheme_letter(ch) && !(ch >= '0' && ch <= '9') && ch != '+' && ch != '-' && ch != '.') return false;
        }
        parts.scheme = text.substr(0, colon);
        cursor = colon + 1;
    }

    const auto hash = text.find('#', cursor);
    const auto question = text.find('?', cursor);
    const std::size_t fragment_end = hash == std::string::npos ? text.size() : hash;
    const bool query_present = question != std::string::npos && question < fragment_end;
    const std::size_t path_end = query_present ? question : fragment_end;
    if (text.compare(cursor, 2, "//") == 0)
    {
        parts.has_authority = true;
        const auto slash = text.find('/', cursor + 2);
        const std::size_t authority_end = slash == std::string::npos ? path_end : std::min(slash, path_end);
        parts.authority = text.substr(cursor + 2, authority_end - cursor - 2);
        cursor = authority_end;
    }
    parts.path = text.substr(cursor, path_end - cursor);
    parts.has_query = query_present;
    if (query_present) parts.query = text.substr(question + 1, fragment_end - question - 1);
    parts.has_fragment = hash != std::string::npos;
    if (parts.has_fragment) parts.fragment = text.substr(hash + 1);
    return true;
}

inline void remove_last_segment(std::string &path)
{
    const auto slash = path.rfind('/');
    if (slash == std::string::npos) path.clear();
    else path.erase(slash);
}

// RFC 3986 section 5.2.4. Apply this only to paths; query/fragment data and
// percent-encoded dots or slashes must not participate in segment removal.
inline std::string remove_dot_segments(std::string input)
{
    std::string output;
    while (!input.empty())
    {
        if (input.compare(0, 3, "../") == 0) input.erase(0, 3);
        else if (input.compare(0, 2, "./") == 0) input.erase(0, 2);
        else if (input.compare(0, 3, "/./") == 0) input.erase(0, 2);
        else if (input == "/.") input = "/";
        else if (input.compare(0, 4, "/../") == 0)
        {
            input.erase(0, 3);
            remove_last_segment(output);
        }
        else if (input == "/..")
        {
            input = "/";
            remove_last_segment(output);
        }
        else if (input == "." || input == "..") input.clear();
        else
        {
            const auto slash = input.find('/', input[0] == '/' ? 1 : 0);
            const std::size_t bytes = slash == std::string::npos ? input.size() : slash;
            output.append(input, 0, bytes);
            input.erase(0, bytes);
        }
    }
    return output;
}

inline std::string serialize(const Parts &parts)
{
    std::string result;
    if (!parts.scheme.empty()) result = parts.scheme + ':';
    if (parts.has_authority) result += "//" + parts.authority;
    result += parts.path;
    if (parts.has_query) result += '?' + parts.query;
    if (parts.has_fragment) result += '#' + parts.fragment;
    return result;
}

} // namespace browser_url_detail

// Resolve without iostreams: the freestanding browser does not provide the
// glibc locale objects required by libstdc++'s stringstream constructor.
inline std::string browser_resolve_url(const std::string &base, const std::string &reference)
{
    using namespace browser_url_detail;
    Parts parent, relative, result;
    if (!parse(base, parent) || !parse(reference, relative)) return {};
    if (!relative.scheme.empty())
    {
        result = relative;
        // Opaque URIs (for example data:) do not have hierarchical paths.
        if (result.has_authority || (!result.path.empty() && result.path[0] == '/'))
            result.path = remove_dot_segments(result.path);
        return serialize(result);
    }
    result.scheme = parent.scheme;
    result.has_fragment = relative.has_fragment;
    result.fragment = relative.fragment;
    if (relative.has_authority)
    {
        result.has_authority = true;
        result.authority = relative.authority;
        result.path = remove_dot_segments(relative.path);
        result.has_query = relative.has_query;
        result.query = relative.query;
    }
    else
    {
        result.has_authority = parent.has_authority;
        result.authority = parent.authority;
        if (relative.path.empty())
        {
            result.path = parent.path;
            result.has_query = relative.has_query || parent.has_query;
            result.query = relative.has_query ? relative.query : parent.query;
        }
        else
        {
            if (relative.path[0] == '/') result.path = remove_dot_segments(relative.path);
            else
            {
                std::string merged;
                if (parent.has_authority && parent.path.empty()) merged = "/";
                else
                {
                    const auto slash = parent.path.rfind('/');
                    if (slash != std::string::npos) merged = parent.path.substr(0, slash + 1);
                }
                result.path = remove_dot_segments(merged + relative.path);
            }
            result.has_query = relative.has_query;
            result.query = relative.query;
        }
    }
    return serialize(result);
}
