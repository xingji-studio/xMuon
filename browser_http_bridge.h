#pragma once

// Primitive C ABI types isolate hosted browser code from the freestanding
// XAPI headers, which have different stdint and size_t definitions.
typedef unsigned long long BrowserHttpHandle;

typedef struct BrowserHttpInfo
{
    unsigned int state;
    unsigned int status_code;
    unsigned int tls_failure;
    unsigned int verify_flags;
    long long    result;
    unsigned int tls_state;
    int          tls_backend_error;
    char         final_url[4096];
} BrowserHttpInfo;

#ifdef __cplusplus
extern "C" {
#endif
long long browser_http_start(const char *url, unsigned long long max_bytes, unsigned int timeout_ms,
                              const char *ca_file, BrowserHttpHandle *handle);
long long browser_http_start_with_cookie(const char *url, unsigned long long max_bytes, unsigned int timeout_ms,
                                          const char *ca_file, const char *cookie, BrowserHttpHandle *handle);
long long browser_http_header_at(BrowserHttpHandle handle, const char *name, unsigned int index, char *value,
                                 unsigned int capacity, unsigned int *required);
unsigned long long browser_http_unix_seconds(void);
long long browser_http_read(BrowserHttpHandle handle, void *data, unsigned int capacity);
long long browser_http_step(BrowserHttpHandle handle);
long long browser_http_info(BrowserHttpHandle handle, BrowserHttpInfo *info);
long long browser_http_header(BrowserHttpHandle handle, const char *name, char *value, unsigned int capacity,
                               unsigned int *required);
void browser_http_cancel(BrowserHttpHandle handle);
void browser_http_close(BrowserHttpHandle handle);
void browser_http_close_idle(void);
// Authorize only after a TLS trust-file denial, following the SDK's system CA
// paths. Never search past a denial, invalid file, or I/O error.
long long browser_http_authorize_ca(char *path, unsigned int capacity);
unsigned long long browser_http_now_ms(void);
void browser_http_pause(void);
#ifdef __cplusplus
}
#endif
