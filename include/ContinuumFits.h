#pragma once

#include <cstdint>
#include <string>

#include <fitsio.h>

class ContinuumFits
{
public:
    ContinuumFits();
    ~ContinuumFits();

    bool create(const std::string &filename,
                double integration_time_sec);

    // Explicit integration start, not centre minus half the live exposure.
    bool write(uint64_t start_ns,
               double total_power,
               double exposure_sec,
               int noise_state);

    void close();

    bool is_open() const
    {
        return m_fptr != nullptr;
    }

private:
    fitsfile *m_fptr;

    int m_status;
    uint64_t m_row;

    double m_integration_time_sec;
};
