/* sdfits.h */
#ifndef _SDFITS_H
#define _SDFITS_H
#include "fitsio.h"
#include <string>
#include <cstring>
#include <iostream>

// The following is the max file length in GB
// #define SDFITS_MAXFILELEN 1L

// The following is the template file to use to create a PSRFITS file.
// Path is relative to VEGAS_DIR environment variable.
// #define SDFITS_TEMPLATE "src/vegas_SDFITS_template.txt"

class PrimaryHdrInfo
{
public:
    char date[16]; // Date file was created (dd/mm/yy)

    PrimaryHdrInfo()
    {
        std::strcpy(date, "");
    }

    PrimaryHdrInfo(const char *input_date)
    {
        std::strncpy(date, input_date, 15);
    }
};

class HdrInfo
{
public:
    char telescope[16]; // Telescope used
    double bandwidth;   // Bandwidth of the entire backend
    double freqres;     // Width of each spectral channel in the file
    char date_obs[16];  // Date of observation (dd/mm/yy)
    double tsys;        // System temperature

    char projid[16];   // The project ID
    char frontend[16]; // Frontend used
    double obsfreq;    // Centre frequency for observation
    double scan;       // Scan number (float)

    char instrument[16]; // Backend or instrument used
    char cal_mode[16];   // Cal mode (OFF, SYNC, EXT1, EXT2)
    double cal_freq;     // Cal modulation frequency (Hz)
    double cal_dcyc;     // Cal duty cycle (0-1)
    double cal_phs;      // Cal phase (wrt start time)
    int npol;            // Number of antenna polarisations (normally 2)
    int nchan;           // Number of spectral bins per sub-band
    double chan_bw;      // Width of each spectral bin

    int nsubband;    // Number of sub-bands
    double efsampfr; // Effective sampling frequency (after decimation)
    double fpgaclk;  // FPGA clock rate [Hz]
    double hwexposr; // Duration of fixed integration on FPGA/GPU [s]
    double filtnep;  // PFB filter noise-equivalent parameter
    double sttmjd;   // Observation start time [double MJD]
    
public:
    HdrInfo()
    {
        std::memset(telescope, 0, sizeof(telescope));
        std::memset(date_obs, 0, sizeof(date_obs));
        std::memset(projid, 0, sizeof(projid));
        std::memset(frontend, 0, sizeof(frontend));
        std::memset(instrument, 0, sizeof(instrument));
        std::memset(cal_mode, 0, sizeof(cal_mode));

        bandwidth = 0.0;
        freqres = 0.0;
        tsys = 0.0;
        obsfreq = 0.0;
        scan = 0.0;
        cal_freq = 0.0;
        cal_dcyc = 0.0;
        cal_phs = 0.0;
        npol = 4;
        nchan = 0;
        chan_bw = 0.0;
        nsubband = 0;
        efsampfr = 0.0;
        fpgaclk = 0.0;
        hwexposr = 0.0;
        filtnep = 0.0;
        sttmjd = 0.0;
    }

    HdrInfo(const char *telescope, double bandwidth, double freqres, const char *date_obs, double tsys,
            const char *projid, const char *frontend, double obsfreq, double scan, const char *instrument,
            const char *cal_mode, double cal_freq, double cal_dcyc, double cal_phs, int npol, int nchan,
            double chan_bw, int nsubband, double efsampfr, double fpgaclk, double hwexposr, double filtnep,
            double sttmjd)
    {
        std::strncpy(this->telescope, telescope, sizeof(this->telescope) - 1);
        std::strncpy(this->date_obs, date_obs, sizeof(this->date_obs) - 1);
        std::strncpy(this->projid, projid, sizeof(this->projid) - 1);
        std::strncpy(this->frontend, frontend, sizeof(this->frontend) - 1);
        std::strncpy(this->instrument, instrument, sizeof(this->instrument) - 1);
        std::strncpy(this->cal_mode, cal_mode, sizeof(this->cal_mode) - 1);

        this->bandwidth = bandwidth;
        this->freqres = freqres;
        this->tsys = tsys;
        this->obsfreq = obsfreq;
        this->scan = scan;
        this->cal_freq = cal_freq;
        this->cal_dcyc = cal_dcyc;
        this->cal_phs = cal_phs;
        this->npol = npol;
        this->nchan = nchan;
        this->chan_bw = chan_bw;
        this->nsubband = nsubband;
        this->efsampfr = efsampfr;
        this->fpgaclk = fpgaclk;
        this->hwexposr = hwexposr;
        this->filtnep = filtnep;
        this->sttmjd = sttmjd;
    }
};

class sdfits_data_columns
{
public:
    double time;                    // MJD start of integration (from system time)
    unsigned long int time_counter; // FPGA time counter at start of integration
    int integ_num;                  // The integration number (indicates a specific integ. period)
    float exposure;                 // Effective integration time (seconds)
    char object[16];                // Object being viewed
    float azimuth;                  // Commanded azimuth
    float elevation;                // Commanded elevation
    float bmaj;                     // Beam major axis length (deg)
    float bmin;                     // Beam minor axis length (deg)
    float bpa;                      // Beam position angle (deg)

    int accumid; // ID of the accumulator from where the spectrum came
    int sttspec; // SPECTRUM_COUNT of the first spectrum in the integration
    int stpspec; // SPECTRUM_COUNT of the last spectrum in the integration

    float centre_freq_idx; // Index of centre frequency bin
    double centre_freq[8]; // Frequency at centre of each sub-band
    double ra;             // RA mid-integration
    double dec;            // DEC mid-integration

    char data_len[16];   // Length of the data array
    char data_dims[16];  // Data matrix dimensions
    unsigned char *data; // Pointer to the raw data itself
    // ---- New fields to record noise source (cal) state per row ----
    int cal_on;        // 0 = noise diode OFF, 1 = ON (per-row state)
    double cal_phase;  // optional: calibration phase (if applicable), otherwise 0.0
public:
    // Default constructor
    sdfits_data_columns()
        : time(0.0), time_counter(0), integ_num(0), exposure(0.0), azimuth(0.0),
          elevation(0.0), bmaj(0.0), bmin(0.0), bpa(0.0), accumid(0),
          sttspec(0), stpspec(0), centre_freq_idx(0.0), ra(0.0), dec(0.0), data(nullptr)
          ,cal_on(0), cal_phase(0.0)
    {
        std::memset(object, 0, sizeof(object));
        std::memset(data_len, 0, sizeof(data_len));
        std::memset(data_dims, 0, sizeof(data_dims));
        std::memset(centre_freq, 0, sizeof(centre_freq));
    }

    // Parameterized constructor
    sdfits_data_columns(double t, unsigned long int counter, int num, float exp, const char *obj,
                        float az, float el, float bm, float bn, float bp, int acc_id,
                        int start_spec, int end_spec, float freq_idx, const double *freq,
                        double r, double d, const char *d_len, const char *d_dims, unsigned char *raw_data,
                    int calon = 0, double calph = 0.0)
        : time(t), time_counter(counter), integ_num(num), exposure(exp), azimuth(az),
          elevation(el), bmaj(bm), bmin(bn), bpa(bp), accumid(acc_id),
          sttspec(start_spec), stpspec(end_spec), centre_freq_idx(freq_idx), ra(r), dec(d), data(raw_data),
          cal_on(calon), cal_phase(calph)
    {
        std::strncpy(object, obj, sizeof(object) - 1);
        std::memset(data_len, 0, sizeof(data_len));
        std::memset(data_dims, 0, sizeof(data_dims));
        std::strncpy(data_len, d_len, sizeof(data_len) - 1);
        std::strncpy(data_dims, d_dims, sizeof(data_dims) - 1);
        std::memcpy(centre_freq, freq, sizeof(centre_freq));
    }

    // Destructor
    ~sdfits_data_columns()
    {
        if (data != nullptr)
        {
            delete[] data;
        }
    }
};

class sdfits
{
public:
    char basefilename[200]; // The base filename from which to build the true filename
    char filename[210];     // Filename of the current PSRFITs file
    long long N;            // Current number of spectra written
    double T;               // Current duration of the observation written
    int filenum;            // The current number of the file in the scan (1-offset)
    int new_file;           // Indicates that a new file must be created.
    int rownum;             // The current data row number to be written (1-offset)
    int tot_rows;           // The total number of data rows written so far
    int rows_per_file;      // The maximum number of data rows per file
    int status;             // The CFITSIO status value
    fitsfile *fptr;         // The CFITSIO file structure
    int multifile;          // Write multiple output files
    int quiet;              // Be quiet about writing each subint
    char mode;              // Read (r) or write (w).
    PrimaryHdrInfo primary_hdr;
    HdrInfo hdr;
    sdfits_data_columns data_columns;

    sdfits() : N(0), T(0.0), filenum(0), new_file(0), rownum(0), tot_rows(0),
               rows_per_file(0), status(0),
               fptr(nullptr),
               multifile(0), quiet(0), mode('w')
    {
        std::memset(basefilename, 0, sizeof(basefilename));
        std::memset(filename, 0, sizeof(filename));
    }

    int sdfits_create();
    int sdfits_close();
    int sdfits_write_subint();
};

#endif