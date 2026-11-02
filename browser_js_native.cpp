// Hosted-library gaps required by the browser's JavaScript engine. Keep these
// adapters local to this app rather than changing the shared XAPI ABI.
#include <cstdint>
#include <ctime>
#include <cstring>

extern "C" long lrint(double value)
{
    // SSE converts using the current rounding mode, including ties-to-even.
    long result;
    asm("cvtsd2si %1, %0" : "=r"(result) : "x"(value));
    return result;
}

extern "C" struct tm *localtime_r(const time_t *time, struct tm *output)
{
    if (!time || !output) return nullptr;
    // The native user runtime exports no timezone database. Use UTC, including
    // a defined tm_gmtoff, rather than consuming an uninitialized hosted field.
    std::memset(output, 0, sizeof(*output));
    std::int64_t days = *time / 86400, seconds = *time % 86400;
    if (seconds < 0) { seconds += 86400; --days; }
    output->tm_sec = seconds % 60;
    output->tm_min = seconds / 60 % 60;
    output->tm_hour = seconds / 3600;
    output->tm_wday = (days + 4) % 7;
    if (output->tm_wday < 0) output->tm_wday += 7;
    // Gregorian civil date from a Unix day, valid on either side of the epoch.
    const std::int64_t shifted = days + 719468;
    const auto era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
    const auto day_of_era = static_cast<unsigned>(shifted - era * 146097);
    const auto year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    auto year = static_cast<std::int64_t>(year_of_era) + era * 400;
    const auto day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const auto month_prime = (5 * day_of_year + 2) / 153;
    const auto day = day_of_year - (153 * month_prime + 2) / 5 + 1;
    const auto month = month_prime + (month_prime < 10 ? 3 : -9);
    year += month <= 2;
    output->tm_year = static_cast<int>(year - 1900);
    output->tm_mon = static_cast<int>(month - 1);
    output->tm_mday = day;
    static const int month_start[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    output->tm_yday = month_start[output->tm_mon] + day - 1;
    if (month > 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ++output->tm_yday;
    return output;
}
