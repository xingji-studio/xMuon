#include "browser_http_bridge.h"
#include "../xapi/include/xhttp.h"
#include "../xapi/include/xpermissions.h"
#include "../xapi/include/libsys.h"
#include "../xapi/include/xposix/errno.h"
#include "../xapi/include/xposix/time.h"
#include "../xapi/include/string.h"

static_assert(sizeof(BrowserHttpHandle) == sizeof(XHttpHandle), "HTTP handle ABI");
static_assert(sizeof(BrowserHttpInfo) == 4128, "browser HTTP information ABI");

extern "C" void xapi_Sleep(unsigned long long ms);

extern "C" long long browser_http_start(const char *url, unsigned long long max_bytes, unsigned int timeout_ms,
                                         const char *ca_file, BrowserHttpHandle *handle)
{
    return browser_http_start_with_cookie(url, max_bytes, timeout_ms, ca_file, nullptr, handle);
}

extern "C" long long browser_http_start_with_cookie(const char *url, unsigned long long max_bytes,
                                                     unsigned int timeout_ms, const char *ca_file,
                                                     const char *cookie, BrowserHttpHandle *handle)
{
    if (!handle || !max_bytes || !timeout_ms || timeout_ms > 30000) return -EINVAL;
    *handle = 0;
    const XHttpHeader headers[] = {
        {"User-Agent", "XJ380Browser/0.2"},
        {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
        {"Accept-Encoding", "identity"},
        {"Cookie", cookie ? cookie : ""},
    };
    XHttpRequestDesc request = {};
    request.size = sizeof(request);
    request.version = XHTTP_ABI_VERSION;
    request.url = url;
    request.headers = headers;
    request.header_count = cookie && *cookie ? 4 : 3;
    // The browser follows each hop itself so intermediate Set-Cookie fields
    // are observed and the outgoing Cookie header is selected for that host.
    request.max_redirects = 0;
    request.total_timeout_ms = static_cast<int32_t>(timeout_ms);
    request.max_response_bytes = max_bytes;
    request.flags = XHTTP_REUSE_CONNECTION;
    request.ca_file = ca_file;
    XHttpHandle sdk_handle = 0;
    const XNetResult result = xapi_HttpRequestStart(&request, &sdk_handle);
    *handle = static_cast<BrowserHttpHandle>(sdk_handle);
    return result;
}

extern "C" long long browser_http_read(BrowserHttpHandle handle, void *data, unsigned int capacity)
{
    // Immediate progress keeps browser events on their owning thread.
    return xapi_HttpRead(handle, data, capacity, NULL);
}

extern "C" long long browser_http_step(BrowserHttpHandle handle)
{
    XNetIoOptions io = {sizeof(io), XNET_ABI_VERSION, 16, 0, 0};
    uint32_t events = 0;
    return xapi_HttpWait(handle, XHTTP_BODY_READY | XHTTP_DONE, &io, &events);
}

extern "C" long long browser_http_info(BrowserHttpHandle handle, BrowserHttpInfo *info)
{
    if (!info) return -EINVAL;
    XHttpResponseInfo response = {};
    response.size = sizeof(response);
    response.version = XHTTP_ABI_VERSION;
    XNetResult result = xapi_HttpGetResponseInfo(handle, &response);
    if (result < 0) return result;
    memset(info, 0, sizeof(*info));
    info->state = response.state;
    info->status_code = response.status_code;
    info->result = response.result;
    memcpy(info->final_url, response.final_url, sizeof(info->final_url));
    info->final_url[sizeof(info->final_url) - 1] = '\0';
    XTlsPeerInfo tls = {};
    tls.size = sizeof(tls);
    tls.version = XTLS_ABI_VERSION;
    if (xapi_HttpGetTlsInfo(handle, &tls) == 0)
    {
        info->tls_failure = tls.failure;
        info->verify_flags = tls.verify_flags;
        info->tls_state = tls.state;
        info->tls_backend_error = tls.backend_error;
    }
    return 0;
}

extern "C" long long browser_http_header(BrowserHttpHandle handle, const char *name, char *value,
                                          unsigned int capacity, unsigned int *required)
{
    return browser_http_header_at(handle, name, 0, value, capacity, required);
}

extern "C" long long browser_http_header_at(BrowserHttpHandle handle, const char *name, unsigned int index,
                                             char *value, unsigned int capacity, unsigned int *required)
{
    return xapi_HttpGetHeader(handle, name, index, value, capacity, required);
}

extern "C" unsigned long long browser_http_unix_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) < 0 || now.tv_sec < 0) return 0;
    return static_cast<unsigned long long>(now.tv_sec);
}

extern "C" void browser_http_cancel(BrowserHttpHandle handle)
{
    if (handle) xapi_HttpCancel(handle);
}

extern "C" void browser_http_close(BrowserHttpHandle handle)
{
    if (handle) xapi_HttpClose(handle);
}

extern "C" void browser_http_close_idle(void)
{
    xapi_HttpCloseIdleConnections();
}

extern "C" long long browser_http_authorize_ca(char *out, unsigned int capacity)
{
    if (!out || !capacity) return -EINVAL;
    static const char *const paths[] = {
        "/etc/ssl/certs/ca-certificates.crt", "/etc/ssl/cert.pem",
        "/system/ssl/certs/ca-certificates.crt", "/system/ssl/cert.pem",
    };
    for (const char *path : paths)
    {
        // Probe without requesting permissions on nonexistent fallback paths.
        int64_t fd = static_cast<int64_t>(enter_syscall(SYS_OPEN, (uint64_t)path, 0, 0, 0, 0, 0));
        if (fd == -EACCES)
        {
            if (xj380_auto_file_access(path, XFA_READ) < 0) return -EACCES;
            fd = static_cast<int64_t>(enter_syscall(SYS_OPEN, (uint64_t)path, 0, 0, 0, 0, 0));
        }
        if (fd == -ENOENT || fd == -ENOTDIR) continue;
        if (fd < 0) return fd;
        enter_syscall(SYS_CLOSE, static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);
        const size_t bytes = strlen(path) + 1;
        if (bytes > capacity) return -ENOBUFS;
        memcpy(out, path, bytes);
        return 0;
    }
    return -ENOENT;
}

extern "C" unsigned long long browser_http_now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0 || now.tv_sec < 0 || now.tv_nsec < 0) return ~0ULL;
    return static_cast<unsigned long long>(now.tv_sec) * 1000 +
           static_cast<unsigned long long>(now.tv_nsec) / 1000000;
}

extern "C" void browser_http_pause(void)
{
    // Some protocol transitions don't wait on transport. Yield rather than
    // monopolizing the CPU while waiting for a fragmented or stalled peer.
    xapi_Sleep(1);
}
