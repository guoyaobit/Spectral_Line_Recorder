#include "time_utils.h"
#include "SpectrumTransport.hpp"

#include <cmath>
#include <cstring>

int main()
{
    constexpr uint64_t epoch_ns = 1782864000000000000ULL;
    constexpr uint64_t centre_ns = epoch_ns + 500000000ULL;

    const uint64_t start_ns =
        spectrum_time::integration_start_ns(centre_ns, 1.0);
    if (start_ns != epoch_ns)
        return 1;

    char date_obs[32]{};
    if (!spectrum_time::format_fits_utc(
            start_ns, date_obs, sizeof(date_obs)))
        return 2;
    if (std::strcmp(date_obs, "2026-07-01T00:00:00.000000000") != 0)
        return 3;

    if (std::abs(spectrum_time::unix_ns_to_mjd(start_ns) - 61222.0) >
        1.0e-10)
        return 4;

    static constexpr char crc_input[] = "123456789";
    if (spectrum_crc32c(crc_input, sizeof(crc_input) - 1) != 0xe3069283U)
        return 5;

    return 0;
}
