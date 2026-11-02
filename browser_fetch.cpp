#include "browser_fetch.h"
#include "browser_http_bridge.h"
#include "browser_retry.h"
#include "browser_cookies.h"
#include "browser_content_encoding.h"

#include <cctype>
#include <cstdio>
#include <new>
#include <string>

namespace
{

constexpr long long FETCH_CANCELLED = -125;
constexpr long long FETCH_AGAIN = -11;
constexpr long long FETCH_ACCESS = -13;
constexpr long long FETCH_TIMEOUT = -110;
constexpr long long FETCH_TOO_LARGE = -90;
constexpr unsigned int TLS_FAILURE_TRUST = 1;
constexpr unsigned int HTTP_COMPLETE = 5;
constexpr unsigned int HTTP_FAILED = 6;
browser_cookies::Jar cookie_jar;

static std::string trim_copy(const std::string &value)
{
    std::size_t start = 0, end = value.size();
    while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(start, end - start);
}

static std::string lower_copy(std::string value)
{
    for (char &ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

static bool normalize_url(const std::string &input, std::string &url)
{
    std::string text = trim_copy(input);
    if (text.empty() || text.size() >= 4096) return false;
    if (text.find("://") == std::string::npos) text = "https://" + text;
    const std::size_t scheme_end = text.find("://");
    const std::string scheme = lower_copy(text.substr(0, scheme_end));
    if (scheme != "http" && scheme != "https") return false;
    text.replace(0, scheme_end, scheme);
    const std::size_t authority = scheme_end + 3;
    const std::size_t path = text.find_first_of("/?#", authority);
    const std::size_t fragment = text.find('#', authority);
    const std::size_t end = fragment == std::string::npos ? text.size() : fragment;
    static const char hex[] = "0123456789ABCDEF";
    url.clear();
    for (std::size_t i = 0; i < end; ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch < 32 || ch == 127) return false;
        const bool in_path = path != std::string::npos && i >= path;
        if (ch >= 128 || ch == ' ')
        {
            // DNS names require IDNA. UTF-8 path/query bytes can be encoded
            // losslessly without changing existing percent escapes.
            if (!in_path) return false;
            url.push_back('%');
            url.push_back(hex[ch >> 4]);
            url.push_back(hex[ch & 15]);
        }
        else
            url.push_back(static_cast<char>(ch));
        if (url.size() >= 4096) return false;
    }
    return true;
}

static void parse_content_type(const std::string &header, BrowserFetchResult &result)
{
    const std::size_t semicolon = header.find(';');
    result.content_type = lower_copy(trim_copy(header.substr(0, semicolon)));
    std::size_t pos = semicolon;
    while (pos != std::string::npos && pos < header.size())
    {
        ++pos;
        const std::size_t key_start = pos;
        while (pos < header.size() && header[pos] != '=' && header[pos] != ';') ++pos;
        const std::string key = lower_copy(trim_copy(header.substr(key_start, pos - key_start)));
        if (pos == header.size()) break;
        if (header[pos] != '=') continue;
        ++pos;
        while (pos < header.size() && std::isspace(static_cast<unsigned char>(header[pos]))) ++pos;
        std::string value;
        if (pos < header.size() && (header[pos] == '\'' || header[pos] == '"'))
        {
            const char quote = header[pos++];
            bool closed = false;
            while (pos < header.size())
            {
                char ch = header[pos++];
                if (ch == quote) { closed = true; break; }
                if (ch == '\\' && pos < header.size()) ch = header[pos++];
                value.push_back(ch);
            }
            if (!closed) break;
            while (pos < header.size() && std::isspace(static_cast<unsigned char>(header[pos]))) ++pos;
            if (pos < header.size() && header[pos] != ';') break;
        }
        else
        {
            const std::size_t begin = pos;
            while (pos < header.size() && header[pos] != ';') ++pos;
            value = trim_copy(header.substr(begin, pos - begin));
        }
        if (key == "charset")
        {
            result.charset = lower_copy(trim_copy(value));
            return;
        }
        if (pos >= header.size()) break;
    }
}

class HttpRequest
{
public:
    HttpRequest() = default;
    HttpRequest(const HttpRequest &) = delete;
    HttpRequest &operator=(const HttpRequest &) = delete;
    BrowserHttpHandle handle = 0;
    ~HttpRequest() { close(); }
    void close()
    {
        if (handle) browser_http_close(handle);
        handle = 0;
    }
};

static void fail(BrowserFetchResult &result, long long code, unsigned int tls_failure = 0)
{
    result.ok = false;
    result.error_code = code;
    result.cancelled = code == FETCH_CANCELLED;
    result.error_summary = browser_fetch_error_message(code, tls_failure);
    result.error = result.error_summary;
    if (result.tls_state)
    {
        // Keep native TLS diagnostics before Close destroys the request. This
        // string is written to serial.log, rather than exposed as UI controls.
        char diagnostic[192];
        std::snprintf(diagnostic, sizeof(diagnostic),
                      " [HTTP=%u TLS=%u failure=%u backend=%d verify=0x%x]",
                      result.http_state, result.tls_state, result.tls_failure,
                      result.tls_backend_error, result.tls_verify_flags);
        result.error += diagnostic;
    }
    result.body.clear();
}

static bool keep_running(const BrowserFetchOptions &options)
{
    return !options.keep_running || options.keep_running(options.user_data);
}

static long long read_header_at(BrowserHttpHandle handle, const char *name, unsigned int index, std::string &value)
{
    unsigned int required = 0;
    long long error = browser_http_header_at(handle, name, index, nullptr, 0, &required);
    if (error < 0) return error;
    if (!required || required > 8193) return FETCH_TOO_LARGE;
    value.resize(required);
    error = browser_http_header_at(handle, name, index, &value[0], required, &required);
    if (error < 0) return error;
    value.resize(required - 1);
    return 0;
}

static long long read_header(BrowserHttpHandle handle, const char *name, std::string &value)
{
    const auto error = read_header_at(handle, name, 0, value);
    return error == -2 ? 0 : error;
}

} // namespace

unsigned long long browser_cookie_generation()
{
    return static_cast<unsigned long long>(cookie_jar.generation(browser_http_unix_seconds()));
}

std::string browser_document_cookie(const std::string &url)
{
    return cookie_jar.header(url, url, true, browser_http_unix_seconds(), false, true);
}

void browser_set_document_cookie(const std::string &url, const std::string &value)
{
    cookie_jar.store(url, value, browser_http_unix_seconds(), url, true, false, true);
}

struct BrowserFetchTask::Impl
{
    enum class Phase { Start, Read, Backoff, Done };
    BrowserFetchOptions options;
    BrowserFetchResult response;
    HttpRequest request;
    std::string url, original_url;
    unsigned int redirects = 0;
    bool continuing_redirect = false, headers_collected = false, cookie_sensitive = false;
    bool redirected_cross_site = false;
    unsigned long long cookie_start_generation = 0;
    Phase phase = Phase::Start;
    unsigned int attempts = 0, retries = 0, displayed_seconds = ~0U;
    unsigned long long started = 0, retry_at = 0;
    bool authorized = false;
    char ca_file[128] {};

    Impl(const std::string &input, const BrowserFetchOptions &value) : options(value)
    {
        retries = options.max_retries > 2 ? 2 : options.max_retries;
        if (!normalize_url(input, url) || !options.max_response_bytes)
        {
            fail(response, -22);
            phase = Phase::Done;
        }
        original_url = url;
    }

    void finish_error(long long error, unsigned int tls_failure = 0)
    {
        fail(response, error, tls_failure);
        request.close();
        if (!response.cancelled && attempts <= retries &&
            browser_should_retry(error, response.tls_failure))
        {
            const auto now = browser_http_now_ms();
            if (now == ~0ULL) { fail(response, -5); phase = Phase::Done; return; }
            retry_at = now + browser_retry_delay_ms(attempts - 1);
            phase = Phase::Backoff;
            const unsigned int delay = browser_retry_delay_ms(attempts - 1);
            displayed_seconds = (delay + 999) / 1000;
            notify(BrowserRetryStatus::Phase::Waiting, delay);
        }
        else phase = Phase::Done;
    }

    void notify(BrowserRetryStatus::Phase next, unsigned int remaining)
    {
        if (!options.retry_status) return;
        BrowserRetryStatus status;
        status.phase = next;
        status.attempt = attempts + 1;
        status.max_attempts = retries + 1;
        status.remaining_ms = remaining;
        options.retry_status(response, status, options.user_data);
    }

    void start(bool new_attempt)
    {
        const auto now = browser_http_now_ms();
        if (now == ~0ULL) { finish_error(-5); return; }
        if (new_attempt)
        {
            ++attempts;
            url = original_url;
            redirects = 0;
            redirected_cross_site = false;
            response = BrowserFetchResult();
            response.attempts = attempts;
            response.final_url = url;
            started = now;
            authorized = false;
            ca_file[0] = 0;
        }
        if (now < started) { finish_error(-5); return; }
        if (now - started >= 30000) { finish_error(FETCH_TIMEOUT); return; }
        headers_collected = false;
        const auto cookie = cookie_jar.header(url, options.cookie_site.empty() ? original_url : options.cookie_site,
                                              options.top_level_navigation, browser_http_unix_seconds(),
                                              redirected_cross_site);
        cookie_sensitive = cookie_sensitive || !cookie.empty();
        cookie_start_generation = browser_cookie_generation();
        const auto error = browser_http_start_with_cookie(url.c_str(), options.max_response_bytes,
                                             static_cast<unsigned int>(30000 - (now - started)),
                                             authorized ? ca_file : nullptr, cookie.c_str(), &request.handle);
        if (error < 0) { finish_error(error); return; }
        phase = Phase::Read;
    }

    void collect_cookie_headers()
    {
        if (headers_collected || !request.handle) return;
        // Header queries return EINPROGRESS until a complete, valid final
        // header section exists. Cookies survive later body errors/cancellation.
        for (unsigned int index = 0; index < 64; ++index)
        {
            std::string header;
            const auto error = read_header_at(request.handle, "Set-Cookie", index, header);
            if (error == -2) { headers_collected = true; break; }
            if (error == FETCH_TOO_LARGE) continue; // Ignore oversized individual cookie fields.
            if (error < 0) return;
            cookie_sensitive = cookie_jar.store(url, header, browser_http_unix_seconds(),
                options.cookie_site.empty() ? original_url : options.cookie_site,
                options.top_level_navigation, redirected_cross_site) || cookie_sensitive;
        }
        headers_collected = true;
    }

    void collect(long long error)
    {
        BrowserHttpInfo info {};
        const auto queried = browser_http_info(request.handle, &info);
        if (queried < 0) { finish_error(queried); return; }
        response.status_code = static_cast<int>(info.status_code);
        response.http_state = info.state;
        response.tls_state = info.tls_state;
        response.tls_failure = info.tls_failure;
        response.tls_backend_error = info.tls_backend_error;
        response.tls_verify_flags = info.verify_flags;
        response.final_url = info.final_url;
        if (info.state == HTTP_FAILED && info.result < 0) error = info.result;
        if (error < 0)
        {
            if (error == FETCH_ACCESS && info.tls_failure == TLS_FAILURE_TRUST && !authorized)
            {
                request.close();
                if (!keep_running(options)) { finish_error(FETCH_CANCELLED); return; }
                error = browser_http_authorize_ca(ca_file, sizeof(ca_file));
                if (error < 0) { finish_error(error, TLS_FAILURE_TRUST); return; }
                authorized = true;
                response.body.clear();
                start(false);
                return;
            }
            finish_error(error, info.tls_failure);
            return;
        }
        if (info.state != HTTP_COMPLETE) { finish_error(-71); return; }
        const auto status = info.status_code;
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
        {
            std::string location;
            error = read_header(request.handle, "Location", location);
            if (error < 0) { finish_error(error); return; }
            if (!location.empty())
            {
                if (redirects >= 8) { finish_error(-40); return; }
                std::string next;
                if (!normalize_url(browser_resolve_url(url, location), next)) { finish_error(-22); return; }
                if (browser_cookies::Url(url).secure && !browser_cookies::Url(next).secure)
                { finish_error(FETCH_ACCESS); return; }
                redirected_cross_site = redirected_cross_site || !browser_cookies::same_site(url, next);
                url = std::move(next);
                ++redirects;
                request.close();
                response = BrowserFetchResult();
                response.attempts = attempts;
                response.final_url = url;
                continuing_redirect = true;
                phase = Phase::Start;
                return;
            }
        }
        std::string content_type, encoding, age, vary;
        error = read_header(request.handle, "Content-Type", content_type);
        if (!error) error = read_header(request.handle, "Content-Encoding", encoding);
        if (!error) error = read_header(request.handle, "Cache-Control", response.cache_control);
        if (!error) error = read_header(request.handle, "Age", age);
        if (!error) error = read_header(request.handle, "Vary", vary);
        if (error < 0) { finish_error(error); return; }
        response.cache_vary_star = vary.find('*') != std::string::npos;
        for (char ch : trim_copy(age))
        {
            if (ch < '0' || ch > '9') { response.cache_age = 3600; break; }
            const unsigned int value = response.cache_age * 10 + static_cast<unsigned int>(ch - '0');
            response.cache_age = value > 3600 ? 3600 : value;
        }
        std::string decoding_error;
        if (!browser_decode_content(response.body, encoding, options.max_response_bytes, decoding_error))
        {
            finish_error(-95);
            response.error = response.error_summary = "HTTP content decoding failed: " + decoding_error;
            return;
        }
        parse_content_type(content_type, response);
        // The page cache is URL-based. Do not share cookie-dependent responses
        // between sessions or reuse them after a later Set-Cookie update.
        if (cookie_sensitive || cookie_start_generation != browser_cookie_generation() ||
            lower_copy(vary).find("cookie") != std::string::npos)
            response.cache_control += ", no-store";
        response.ok = true;
        request.close();
        phase = Phase::Done;
    }

    bool poll(bool allow_start)
    {
        if (phase == Phase::Done) return true;
        if (!keep_running(options)) { cancel(); return true; }
        const auto now = browser_http_now_ms();
        if (now == ~0ULL) { finish_error(-5); return phase == Phase::Done; }
        if (phase == Phase::Backoff)
        {
            if (now < retry_at)
            {
                const unsigned int remaining = static_cast<unsigned int>(retry_at - now);
                const unsigned int seconds = (remaining + 999) / 1000;
                if (seconds != displayed_seconds)
                {
                    displayed_seconds = seconds;
                    notify(BrowserRetryStatus::Phase::Waiting, remaining);
                }
                return false;
            }
            if (!allow_start) return false;
            notify(BrowserRetryStatus::Phase::Starting, 0);
            phase = Phase::Start;
            if (!keep_running(options)) { cancel(); return true; }
        }
        if (phase == Phase::Start)
        {
            if (!allow_start) return false;
            const bool new_attempt = !continuing_redirect;
            continuing_redirect = false;
            start(new_attempt);
            if (phase != Phase::Read) return phase == Phase::Done;
        }
        // Bound work per turn. A positive Read can expose more buffered data,
        // but must not starve another request, navigation or the first paint.
        char buffer[8192];
        for (unsigned int reads = 0; reads < 4 && phase == Phase::Read; ++reads)
        {
            if (!keep_running(options)) { cancel(); return true; }
            const auto count = browser_http_read(request.handle, buffer, sizeof(buffer));
            collect_cookie_headers();
            if (count > 0)
            {
                if (static_cast<unsigned long long>(count) > sizeof(buffer) ||
                    static_cast<unsigned long long>(count) > options.max_response_bytes - response.body.size())
                {
                    browser_http_cancel(request.handle);
                    finish_error(FETCH_TOO_LARGE);
                    break;
                }
                response.body.append(buffer, static_cast<std::size_t>(count));
            }
            else if (count == FETCH_AGAIN) break;
            else { collect(count); break; }
            const auto current = browser_http_now_ms();
            if (current == ~0ULL || current < now) { finish_error(-5); break; }
            if (current - now >= 8) break;
        }
        return phase == Phase::Done;
    }

    void cancel()
    {
        if (phase == Phase::Done) return;
        if (request.handle) browser_http_cancel(request.handle);
        request.close();
        fail(response, FETCH_CANCELLED);
        phase = Phase::Done;
    }
};

BrowserFetchTask::BrowserFetchTask(const std::string &url, const BrowserFetchOptions &options)
    : impl_(new Impl(url, options)) {}

BrowserFetchTask::~BrowserFetchTask() = default;

bool BrowserFetchTask::poll(bool allow_start)
{
    try { return impl_->poll(allow_start); }
    catch (const std::bad_alloc &)
    {
        if (impl_->request.handle) browser_http_cancel(impl_->request.handle);
        impl_->finish_error(-12);
        return true;
    }
}

bool BrowserFetchTask::active() const { return impl_->request.handle != 0; }

bool BrowserFetchTask::done() const { return impl_->phase == Impl::Phase::Done; }

void BrowserFetchTask::cancel() { impl_->cancel(); }

const BrowserFetchResult &BrowserFetchTask::result() const { return impl_->response; }

BrowserFetchResult BrowserFetchTask::take_result()
{
    if (!done()) cancel();
    return std::move(impl_->response);
}

BrowserFetchResult browser_fetch_url(const std::string &url, const BrowserFetchOptions &options)
{
    try
    {
        BrowserFetchTask task(url, options);
        while (!task.poll()) browser_http_pause();
        return task.take_result();
    }
    catch (const std::bad_alloc &)
    {
        BrowserFetchResult result;
        fail(result, -12);
        return result;
    }
}
