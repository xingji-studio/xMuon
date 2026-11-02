#pragma once

#include "browser_url.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

// A bounded, process-local cookie jar. Domain cookies are deliberately confined
// to their setting host until the browser ships a public-suffix database. This
// preserves sessions without allowing a site to write sibling-site cookies.
namespace browser_cookies
{

inline std::string lower(std::string text)
{
    for (char &ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}

inline std::string trim(const std::string &text)
{
    auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

struct Url
{
    std::string host, path;
    bool secure = false, valid = false;
    explicit Url(const std::string &text)
    {
        browser_url_detail::Parts parts;
        if (!browser_url_detail::parse(text, parts) || !parts.has_authority) return;
        const auto scheme = lower(parts.scheme);
        if (scheme != "https" && scheme != "http") return;
        secure = scheme == "https";
        host = lower(parts.authority);
        if (host.empty() || host.find('@') != std::string::npos) return;
        if (host.front() == '[')
        {
            auto end = host.find(']');
            if (end == std::string::npos) return;
            host = host.substr(0, end + 1);
        }
        else
        {
            auto port = host.find(':');
            if (port != std::string::npos) host.resize(port);
        }
        if (host.empty() || host.back() == '.') return;
        path = parts.path.empty() ? "/" : parts.path;
        valid = true;
    }
};

inline bool same_site(const std::string &left, const std::string &right)
{
    const Url a(left), b(right);
    return a.valid && b.valid && a.host == b.host && a.secure == b.secure;
}

inline bool path_matches(const std::string &request, const std::string &cookie)
{
    return request == cookie || (request.size() > cookie.size() && request.compare(0, cookie.size(), cookie) == 0 &&
                                (cookie.back() == '/' || request[cookie.size()] == '/'));
}

inline std::uint64_t http_date(const std::string &text)
{
    // Cookie dates are parsed without scanf/timegm: hosted browser objects
    // link to the freestanding runtime rather than the host's libc.
    auto number = [](const std::string &token) {
        if (token.empty() || token.size() > 4) return -1;
        int value = 0;
        for (char ch : token)
        {
            if (ch < '0' || ch > '9') return -1;
            value = value * 10 + ch - '0';
        }
        return value;
    };
    int day = -1, year = -1, mon = -1, hour = -1, minute = -1, second = -1;
    const std::string months = "janfebmaraprmayjunjulaugsepoctnovdec";
    std::size_t cursor = 0;
    while (cursor < text.size())
    {
        cursor = text.find_first_not_of(" ,-/\t", cursor);
        if (cursor == std::string::npos) break;
        const auto end = text.find_first_of(" ,-/\t", cursor);
        const auto token = lower(text.substr(cursor, end - cursor));
        cursor = end == std::string::npos ? text.size() : end;
        const auto colon = token.find(':');
        if (hour < 0 && colon != std::string::npos)
        {
            const auto next = token.find(':', colon + 1);
            if (next == std::string::npos) continue;
            hour = number(token.substr(0, colon));
            minute = number(token.substr(colon + 1, next - colon - 1));
            second = number(token.substr(next + 1));
            continue;
        }
        const int value = number(token);
        if (day < 0 && token.size() <= 2 && value >= 1 && value <= 31) { day = value; continue; }
        if (mon < 0 && token.size() == 3)
        {
            const auto index = months.find(token);
            if (index != std::string::npos && index % 3 == 0) { mon = static_cast<int>(index / 3) + 1; continue; }
        }
        if (year < 0 && token.size() >= 2 && value >= 0) year = value;
    }
    if (year >= 0 && year <= 69) year += 2000;
    else if (year >= 70 && year <= 99) year += 1900;
    if (year < 1601 || year > 9999 || mon < 1 || day < 1 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) return 0;
    const int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day > lengths[mon - 1] + (mon == 2 && leap ? 1 : 0)) return 0;
    // Gregorian civil date -> days relative to the Unix epoch, independent of
    // libc timezone/locale support in the freestanding runtime.
    year -= mon <= 2;
    const int era = year / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * static_cast<unsigned>(mon + (mon > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = static_cast<long long>(era) * 146097 + doe - 719468;
    if (days < 0) return 1; // An accepted pre-epoch date is already expired.
    const auto timestamp = static_cast<std::uint64_t>(days) * 86400 + hour * 3600 + minute * 60 + second;
    return timestamp ? timestamp : 1;
}

class Jar
{
    enum class SameSite { Lax, Strict, None };
    struct Cookie
    {
        std::string name, value, host, path;
        std::uint64_t expires = 0, order = 0;
        bool secure = false, http_only = false;
        SameSite same_site = SameSite::Lax;
    };
    std::vector<Cookie> cookies_;
    std::uint64_t order_ = 0, generation_ = 0;

    void expire(std::uint64_t now)
    {
        const auto previous = cookies_.size();
        cookies_.erase(std::remove_if(cookies_.begin(), cookies_.end(), [now](const Cookie &cookie) {
            return cookie.expires && now >= cookie.expires;
        }), cookies_.end());
        if (cookies_.size() != previous) ++generation_;
    }

public:
    void clear() { if (!cookies_.empty()) ++generation_; cookies_.clear(); order_ = 0; }
    std::uint64_t generation(std::uint64_t now) { expire(now); return generation_; }

    bool store(const std::string &url, const std::string &header, std::uint64_t now,
               const std::string &site = {}, bool top_level = true, bool redirected_cross_site = false,
               bool script_access = false)
    {
        const Url origin(url);
        if (!origin.valid || header.empty() || header.size() > 4096) return false;
        for (unsigned char ch : header) if (ch < 0x20 && ch != '\t') return false;
        const auto end = header.find(';'), equals = header.find('=');
        if (equals == std::string::npos || equals >= end) return false;
        Cookie cookie;
        cookie.name = trim(header.substr(0, equals));
        cookie.value = trim(header.substr(equals + 1, end - equals - 1));
        if (cookie.name.empty()) return false;
        for (unsigned char ch : cookie.name)
            if (ch <= 0x20 || ch >= 0x7f || std::string("()<>@,;:\\\"/[]?={}").find(ch) != std::string::npos)
                return false;
        if (cookie.value.size() >= 2 && cookie.value.front() == '"' && cookie.value.back() == '"')
            cookie.value = cookie.value.substr(1, cookie.value.size() - 2);
        for (unsigned char ch : cookie.value)
            if (ch < 0x21 || ch > 0x7e || ch == '"' || ch == ',' || ch == ';' || ch == '\\') return false;
        cookie.host = origin.host;
        const auto slash = origin.path.rfind('/');
        cookie.path = slash == 0 || slash == std::string::npos ? "/" : origin.path.substr(0, slash);
        bool domain_attribute = false, path_attribute = false, max_age_valid = false, delete_cookie = false;
        std::uint64_t max_age = 0;
        std::size_t cursor = end;
        while (cursor != std::string::npos && cursor < header.size())
        {
            const auto next = header.find(';', cursor + 1);
            const auto attribute = trim(header.substr(cursor + 1, next - cursor - 1));
            const auto separator = attribute.find('=');
            const auto name = lower(trim(attribute.substr(0, separator)));
            const auto value = separator == std::string::npos ? std::string() : trim(attribute.substr(separator + 1));
            if (name == "secure") cookie.secure = true;
            else if (name == "httponly") cookie.http_only = true;
            else if (name == "path" && !value.empty() && value.front() == '/')
            { cookie.path = value; path_attribute = true; }
            else if (name == "domain")
            {
                domain_attribute = true;
                auto domain = lower(value);
                if (!domain.empty() && domain.front() == '.') domain.erase(0, 1);
                if (domain.empty() || domain.back() == '.' ||
                    !(origin.host == domain || (origin.host.size() > domain.size() &&
                      origin.host.compare(origin.host.size() - domain.size(), domain.size(), domain) == 0 &&
                      origin.host[origin.host.size() - domain.size() - 1] == '.'))) return false;
                // Clamp accepted Domain attributes to the response host.
            }
            else if (name == "max-age" && !value.empty())
            {
                std::size_t digit = value.front() == '-' ? 1 : 0;
                bool valid = digit < value.size();
                std::uint64_t age = 0;
                for (; digit < value.size(); ++digit)
                {
                    if (value[digit] < '0' || value[digit] > '9') { valid = false; break; }
                    age = std::min<std::uint64_t>(34560000, age * 10 + value[digit] - '0');
                }
                if (valid)
                {
                    max_age_valid = true;
                    delete_cookie = value.front() == '-' || age == 0;
                    max_age = age;
                }
            }
            else if (name == "expires") cookie.expires = http_date(value);
            else if (name == "samesite")
            {
                const auto mode = lower(value);
                cookie.same_site = mode == "strict" ? SameSite::Strict : mode == "none" ? SameSite::None : SameSite::Lax;
            }
            cursor = next;
        }
        if (max_age_valid) cookie.expires = delete_cookie ? 1 : now + max_age;
        // Without wall-clock time, retain only session cookies. Persistent
        // cookies must not silently become immortal or revive expired state.
        if (!now && cookie.expires && !delete_cookie) return false;
        if (cookie.secure && !origin.secure) return false;
        if (script_access && cookie.http_only) return false;
        if (cookie.same_site == SameSite::None && !cookie.secure) return false;
        if (cookie.name.compare(0, 9, "__Secure-") == 0 && (!cookie.secure || !origin.secure)) return false;
        if (cookie.name.compare(0, 7, "__Host-") == 0 &&
            (!cookie.secure || !origin.secure || domain_attribute || !path_attribute || cookie.path != "/")) return false;
        const bool same = !redirected_cross_site && (site.empty() || same_site(url, site));
        if (!same && !top_level && cookie.same_site != SameSite::None) return false;
        expire(now);
        for (const auto &existing : cookies_)
            if (!origin.secure && existing.secure && existing.host == cookie.host && existing.name == cookie.name &&
                (path_matches(existing.path, cookie.path) || path_matches(cookie.path, existing.path))) return false;
        auto existing = std::find_if(cookies_.begin(), cookies_.end(), [&](const Cookie &item) {
            return item.host == cookie.host && item.name == cookie.name && item.path == cookie.path;
        });
        if (existing != cookies_.end())
        {
            if (script_access && existing->http_only) return false;
            cookie.order = existing->order;
            cookies_.erase(existing);
        }
        else cookie.order = ++order_;
        ++generation_;
        if (delete_cookie || (cookie.expires && cookie.expires <= now)) return true;
        while (cookies_.size() >= 128) cookies_.erase(cookies_.begin());
        std::size_t count = 0;
        for (const auto &item : cookies_) if (item.host == cookie.host) ++count;
        if (count >= 50)
        {
            auto oldest = std::find_if(cookies_.begin(), cookies_.end(), [&](const Cookie &item) {
                return item.host == cookie.host;
            });
            cookies_.erase(oldest);
        }
        cookies_.push_back(std::move(cookie));
        return true;
    }

    std::string header(const std::string &url, const std::string &site, bool top_level, std::uint64_t now,
                       bool redirected_cross_site = false, bool script_access = false)
    {
        const Url target(url);
        if (!target.valid) return {};
        expire(now);
        const bool same = !redirected_cross_site && (site.empty() || same_site(url, site));
        std::vector<const Cookie *> matching;
        for (const auto &cookie : cookies_)
            if (cookie.host == target.host && (!cookie.secure || target.secure) &&
                (!script_access || !cookie.http_only) &&
                (!cookie.expires || now) &&
                path_matches(target.path, cookie.path) &&
                (same || cookie.same_site == SameSite::None || (top_level && cookie.same_site == SameSite::Lax)))
                matching.push_back(&cookie);
        std::stable_sort(matching.begin(), matching.end(), [](const Cookie *a, const Cookie *b) {
            return a->path.size() != b->path.size() ? a->path.size() > b->path.size() : a->order < b->order;
        });
        std::string result;
        for (const auto *cookie : matching)
        {
            const auto size = cookie->name.size() + cookie->value.size() + 1;
            if (result.size() + size + (result.empty() ? 0 : 2) > 8192) continue;
            if (!result.empty()) result += "; ";
            result += cookie->name + "=" + cookie->value;
        }
        return result;
    }
};

} // namespace browser_cookies
