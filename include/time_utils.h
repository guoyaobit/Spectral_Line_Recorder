#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>

namespace spectrum_time
{

constexpr long double NS_PER_SECOND = 1000000000.0L;
constexpr long double SECONDS_PER_DAY = 86400.0L;
constexpr long double UNIX_EPOCH_MJD = 40587.0L;

// The wire protocol timestamp denotes the centre of an integration.  SDFITS
// TIME and DATE-OBS denote the start, so subtract half of the exposure.
inline uint64_t integration_start_ns(uint64_t centre_ns, double exposure_sec)
{
    if (!std::isfinite(exposure_sec) || exposure_sec <= 0.0)
        return centre_ns;

    const long double half_exposure_ns =
        static_cast<long double>(exposure_sec) * NS_PER_SECOND / 2.0L;
    const uint64_t offset_ns = static_cast<uint64_t>(
        std::llround(half_exposure_ns));
    return offset_ns > centre_ns ? 0 : centre_ns - offset_ns;
}

inline double unix_ns_to_mjd(uint64_t timestamp_ns)
{
    return static_cast<double>(
        UNIX_EPOCH_MJD +
        static_cast<long double>(timestamp_ns) /
            (NS_PER_SECOND * SECONDS_PER_DAY));
}

// FITS date-time syntax is CCYY-MM-DDThh:mm:ss[.s...].  FITS timestamps are
// UTC by TIMESYS; a trailing timezone designator is therefore not written.
inline bool format_fits_utc(uint64_t timestamp_ns,
                            char *output,
                            size_t output_size)
{
    if (output == nullptr || output_size < 30)
        return false;

    const std::time_t seconds =
        static_cast<std::time_t>(timestamp_ns / 1000000000ULL);
    const unsigned nanoseconds =
        static_cast<unsigned>(timestamp_ns % 1000000000ULL);
    std::tm utc_tm{};
#if defined(_WIN32)
    if (gmtime_s(&utc_tm, &seconds) != 0)
        return false;
#else
    if (gmtime_r(&seconds, &utc_tm) == nullptr)
        return false;
#endif

    const int written = std::snprintf(
        output, output_size,
        "%04d-%02d-%02dT%02d:%02d:%02d.%09u",
        utc_tm.tm_year + 1900, utc_tm.tm_mon + 1, utc_tm.tm_mday,
        utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec, nanoseconds);
    return written > 0 && static_cast<size_t>(written) < output_size;
}

} // namespace spectrum_time
