#pragma once

#include <cstddef>
#include <memory>
#include <string>

struct BrowserFetchResult;

struct BrowserRetryStatus
{
    enum class Phase
    {
        Waiting,
        Starting
    };
    Phase        phase = Phase::Waiting;
    unsigned int attempt = 0; // One-based upcoming attempt, including the initial request.
    unsigned int max_attempts = 0;
    unsigned int remaining_ms = 0;
};

struct BrowserFetchOptions
{
    std::size_t max_response_bytes = 8 * 1024 * 1024;
    unsigned int max_retries = 2; // Clamped to two; only transient GET failures are retried.
    // Initiating document and request kind determine SameSite cookie access.
    std::string cookie_site;
    bool top_level_navigation = false;
    // Called between SDK calls, on the caller's thread. May pump toolbar and
    // cancellation events, but must not reenter document operations.
    bool (*keep_running)(void *) = nullptr;
    // Called on the same thread, once per displayed countdown second and when
    // the next attempt starts. The failed result is valid during the callback.
    void (*retry_status)(const BrowserFetchResult &, const BrowserRetryStatus &, void *) = nullptr;
    void *user_data = nullptr;
};

struct BrowserFetchResult
{
    // HTTP error pages are valid responses. ok describes transport/protocol
    // completion; inspect status_code before caching a resource.
    bool         ok = false;
    bool         cancelled = false;
    long long    error_code = 0;
    int          status_code = 0;
    unsigned int http_state = 0, tls_state = 0, tls_failure = 0, tls_verify_flags = 0;
    int          tls_backend_error = 0;
    unsigned int attempts = 1;
    std::string error_summary;
    std::string error;
    std::string final_url;
    std::string content_type;
    std::string charset;
    std::string cache_control;
    unsigned int cache_age = 0;
    bool cache_vary_star = false;
    std::string body;
};

inline std::string browser_fetch_error_message(long long code, unsigned int tls_failure, bool english = false)
{
    if (code == -125) return english ? "Request cancelled" : "请求已取消";
    if (tls_failure == 1)
        return english ? "TLS CA bundle unavailable or access denied" : "TLS CA 证书包不可用或访问被拒绝";
    if (tls_failure == 2) return english ? "TLS certificate verification failed" : "TLS 证书验证失败";
    if (tls_failure == 3) return english ? "TLS random source unavailable" : "TLS 随机数源不可用";
    if (tls_failure == 4)
        return english ? "Invalid system time for TLS verification" : "系统时间无效，无法验证 TLS 证书";
    if (tls_failure == 5) return english ? "TLS protocol negotiation failed" : "TLS 协议协商失败";
    switch (code)
    {
    case -110: return english ? "Request timed out" : "请求超时";
    case -90: return english ? "Response exceeds size limit" : "响应超过大小限制";
    case -104: return english ? "Connection reset by peer" : "连接被对端重置";
    case -103: return english ? "Connection aborted" : "连接意外中断";
    case -32: return english ? "Connection closed while sending" : "发送时连接已断开";
    case -100: return english ? "Network is down" : "网络连接不可用";
    case -101: return english ? "Network unreachable" : "无法连接到网络";
    case -111: return english ? "Connection refused" : "服务器拒绝连接";
    case -113: return english ? "Server unreachable" : "无法连接到服务器";
    case -22: return english ? "Invalid URL or unsupported protocol" : "URL 无效或协议不支持";
    case -71: return english ? "Invalid HTTP response" : "服务器返回了无效的 HTTP 响应";
    case -95: return english ? "Unsupported response encoding or protocol" : "不支持的响应编码或协议";
    case -13: return english ? "Access denied" : "访问被拒绝";
    case -12: return english ? "Not enough memory" : "内存不足";
    default:
        return std::string(english ? "Network request failed (" : "网络请求失败 (") + std::to_string(code) + ")";
    }
}

// Advances on cookie changes and expiry, for invalidating URL-only caches.
unsigned long long browser_cookie_generation();
std::string browser_document_cookie(const std::string &url);
void browser_set_document_cookie(const std::string &url, const std::string &value);

BrowserFetchResult browser_fetch_url(const std::string &input_url,
                                     const BrowserFetchOptions &options = BrowserFetchOptions());

// Cooperative GET. poll() never waits for socket readiness or retry timers;
// the owner can advance several requests between input and paint operations.
class BrowserFetchTask
{
public:
    explicit BrowserFetchTask(const std::string &url, const BrowserFetchOptions &options = BrowserFetchOptions());
    ~BrowserFetchTask();
    BrowserFetchTask(const BrowserFetchTask &) = delete;
    BrowserFetchTask &operator=(const BrowserFetchTask &) = delete;
    bool poll(bool allow_start = true);
    bool active() const;
    bool done() const;
    void cancel();
    const BrowserFetchResult &result() const;
    BrowserFetchResult take_result();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
