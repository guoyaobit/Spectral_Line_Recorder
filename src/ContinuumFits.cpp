#include "ContinuumFits.h"

#include <cstdio>
#include <cstdlib>

namespace
{

constexpr double UNIX_MJD = 40587.0;
constexpr double NS_PER_SEC = 1.0e9;
constexpr double SEC_PER_DAY = 86400.0;

}

ContinuumFits::ContinuumFits()
    : m_fptr(nullptr),
      m_status(0),
      m_row(0)
{
}

ContinuumFits::~ContinuumFits()
{
    close();
}
bool ContinuumFits::create(const std::string &filename,
                           double integration_time_sec)
{
    close();

    m_status = 0;
    m_row = 0;
    m_integration_time_sec = integration_time_sec;

    std::string output_name = filename;

    if (output_name.empty() || output_name[0] != '!')
        output_name = "!" + output_name;

    fits_create_file(
        &m_fptr,
        output_name.c_str(),
        &m_status);

    if (m_status)
    {
        fits_report_error(stderr, m_status);
        m_fptr = nullptr;
        return false;
    }

    // Primary HDU
    fits_create_img(
        m_fptr,
        BYTE_IMG,
        0,
        nullptr,
        &m_status);

    if (m_status)
    {
        fits_report_error(stderr, m_status);
        close();
        return false;
    }

    /*
     * Integration time
     */
    fits_update_key(
        m_fptr,
        TDOUBLE,
        "INTTIME",
        &m_integration_time_sec,
        "Integration time [s]",
        &m_status);

    /*
     * Continuum Binary Table
     */
    char *ttype[] =
    {
        const_cast<char *>("TIME"),
        const_cast<char *>("EXPOSURE"),
        const_cast<char *>("DATA"),
        const_cast<char *>("CAL_ON")
    };

    char *tform[] =
    {
        const_cast<char *>("1D"),
        const_cast<char *>("1D"),
        const_cast<char *>("1D"),
        const_cast<char *>("1J")
    };

    char *tunit[] =
    {
        const_cast<char *>("d"),
        const_cast<char *>("s"),
        const_cast<char *>(""),
        const_cast<char *>("")
    };

    char extname[] = "CONTINUUM";

    fits_create_tbl(
        m_fptr,
        BINARY_TBL,
        0,
        4,
        ttype,
        tform,
        tunit,
        extname,
        &m_status);

    if (m_status)
    {
        fits_report_error(stderr, m_status);
        close();
        return false;
    }

    /*
     * Continuum data:
     *
     * NCHAN    = 1
     * NPOL     = 1
     * NSUBBAND = 1
     */
    int nchan = 1;
    int npol = 1;
    int nsubband = 1;

    fits_update_key(
        m_fptr,
        TINT,
        "NCHAN",
        &nchan,
        "Continuum channel",
        &m_status);

    fits_update_key(
        m_fptr,
        TINT,
        "NPOL",
        &npol,
        "Total power",
        &m_status);

    fits_update_key(
        m_fptr,
        TINT,
        "NSUBBAND",
        &nsubband,
        "Aggregated continuum",
        &m_status);

    if (m_status)
    {
        fits_report_error(stderr, m_status);
        close();
        return false;
    }

    fits_flush_file(m_fptr, &m_status);

    return m_status == 0;
}

bool ContinuumFits::write(uint64_t timestamp_ns,
                          double total_power,
                          double exposure_sec,
                          int noise_state)
{
    if (m_fptr == nullptr)
        return false;

    m_status = 0;

    ++m_row;

    /*
     * Unix timestamp(ns) -> MJD
     *
     * MJD = 40587 + Unix seconds / 86400
     */
    const double time_mjd =
        UNIX_MJD +
        static_cast<double>(timestamp_ns) /
        NS_PER_SEC /
        SEC_PER_DAY;

    /*
     * TIME
     */
    fits_write_col(
        m_fptr,
        TDOUBLE,
        1,
        static_cast<long>(m_row),
        1,
        1,
        const_cast<double *>(&time_mjd),
        &m_status);

    /*
     * EXPOSURE
     */
    fits_write_col(
        m_fptr,
        TDOUBLE,
        2,
        static_cast<long>(m_row),
        1,
        1,
        &exposure_sec,
        &m_status);

    /*
     * DATA
     *
     * 连续谱：
     *
     * DATA = total_power
     */
    fits_write_col(
        m_fptr,
        TDOUBLE,
        3,
        static_cast<long>(m_row),
        1,
        1,
        &total_power,
        &m_status);

    /*
     * CAL_ON
     */
    fits_write_col(
        m_fptr,
        TINT,
        4,
        static_cast<long>(m_row),
        1,
        1,
        &noise_state,
        &m_status);
    fits_flush_file(m_fptr, &m_status);
    if (m_status)
    {
        fits_report_error(stderr, m_status);
        return false;
    }

    return true;
}
void ContinuumFits::close()
{
    if (m_fptr == nullptr)
        return;

    int status = 0;

    fits_close_file(
        m_fptr,
        &status);

    if (status)
        fits_report_error(stderr, status);

    m_fptr = nullptr;
}