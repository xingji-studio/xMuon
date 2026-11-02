#pragma once

inline bool browser_should_retry(long long error_code, unsigned tls_failure)
{
    // Match XTLS_FAILURE_NONE/TRANSPORT in the SDK's primitive bridge ABI.
    // A transport errno must never make certificate or TLS setup failures retryable.
    if (tls_failure != 0U && tls_failure != 6U) return false;

    switch (error_code)
    {
    case -104: // ECONNRESET
    case -110: // ETIMEDOUT
    case -103: // ECONNABORTED
    case -32:  // EPIPE
    case -100: // ENETDOWN
    case -101: // ENETUNREACH
    case -111: // ECONNREFUSED
    case -113: // EHOSTUNREACH
        return true;
    default:
        return false;
    }
}

inline unsigned browser_retry_delay_ms(unsigned retry_index0based)
{
    // Two retries allow a short network interruption to clear without looping forever.
    static const unsigned delays[] = {2000U, 5000U};
    return retry_index0based < sizeof(delays) / sizeof(delays[0]) ? delays[retry_index0based] : 0U;
}
