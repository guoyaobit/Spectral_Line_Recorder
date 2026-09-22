/* write_psrfits.c */
#define _ISOC99_SOURCE // For long double strtold
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "sdfits.h"
#include <unistd.h>
#define SDFITS_TEMPLATE "vegas_SDFITS_template.txt"

#define DEBUGOUT 0

int sdfits::sdfits_create()
{
    int itmp;
    char ctmp[40];

    // Initialize the key variables if needed
    if (new_file == 1)
    {
        // first time writing to the file
        // Create the output directory if needed
        char datadir[1024];
        strncpy(datadir, basefilename, 1023);
        char *last_slash = strrchr(datadir, '/');
        if (last_slash != NULL && last_slash != datadir)
        {
            *last_slash = '\0';
            printf("Using directory '%s' for output.\n", datadir);
            char cmd[1048];
            sprintf(cmd, "mkdir -m 1777 -p %s", datadir);
            system(cmd);
        }
        new_file = 0;
    }
    rownum = 1;

    // basefilename is a stem. Be tolerant of older callers that included the
    // extension so the generated name never contains ".sdfits_0001.sdfits".
    std::string filename_stem(basefilename);
    const std::string extension = ".sdfits";
    if (filename_stem.size() >= extension.size() &&
        filename_stem.compare(filename_stem.size() - extension.size(),
                              extension.size(), extension) == 0)
    {
        filename_stem.erase(filename_stem.size() - extension.size());
    }

    // Continue after files left by an earlier recorder process. Reusing
    // _0001 after a restart makes CFITSIO reject creation and stops storage.
    int filename_length = 0;
    do
    {
        filenum++;
        filename_length = snprintf(filename, sizeof(filename), "%s_%04d.sdfits",
                                   filename_stem.c_str(), filenum);
        if (filename_length < 0 ||
            static_cast<size_t>(filename_length) >= sizeof(filename))
        {
            fprintf(stderr, "SDFITS output filename is too long.\n");
            return 1;
        }
    } while (access(filename, F_OK) == 0);

    // Create basic FITS file from our template
    // char *vegas_dir = getenv("VEGAS_DIR");
    char vegas_dir[256];
    getcwd(vegas_dir, sizeof(vegas_dir));
    char template_file[1024];
    // if (vegas_dir == NULL)
    // {
    //     fprintf(stderr,
    //             "Error: VEGAS_DIR environment variable not set, exiting.\n");
    //     exit(1);
    // }

    // printf("Opening file '%s'\n", filename);
    sprintf(template_file, "%s/%s", vegas_dir, SDFITS_TEMPLATE);
    // printf("%s\n",template_file);
    fits_create_template(&(fptr), filename, template_file, &status);

    // Check to see if file was successfully created
    if (status)
    {
        fprintf(stderr, "Error creating sdfits file from template.\n");
        fits_report_error(stderr, status);
        return status;
    }

    // Go to the primary HDU
    fits_movabs_hdu(fptr, 1, NULL, &status);

    // Update the keywords that need it
    fits_get_system_time(ctmp, &itmp, &status); // date the file was written
    fits_update_key(fptr, TSTRING, "DATE", ctmp, NULL, &status);

    // Go to the SINGLE DISH HDU
    // const char* str = "SINGLE DISH";
    fits_movnam_hdu(fptr, BINARY_TBL, "SINGLE DISH", 0, &status);

    // Update the keywords that need it
    fits_update_key(fptr, TSTRING, "TELESCOP", hdr.telescope, NULL, &status);
    fits_update_key(fptr, TDOUBLE, "BANDWID", &(hdr.bandwidth), NULL, &status);
    fits_update_key(fptr, TSTRING, "DATE-OBS", hdr.date_obs, NULL, &status);
    fits_update_key(fptr, TDOUBLE, "TSYS", &(hdr.tsys), NULL, &status);

    fits_update_key(fptr, TSTRING, "PROJID", hdr.projid, NULL, &status);
    fits_update_key(fptr, TSTRING, "FRONTEND", hdr.frontend, NULL, &status);
    fits_update_key(fptr, TDOUBLE, "OBSFREQ", &(hdr.obsfreq), NULL, &status);
    fits_update_key(fptr, TDOUBLE, "SCAN", &(hdr.scan), NULL, &status);

    fits_update_key(fptr, TSTRING, "INSTRUME", hdr.instrument, NULL, &status);
    fits_update_key(fptr, TSTRING, "CAL_MODE", hdr.cal_mode, NULL, &status);
    if (strcmp("OFF", hdr.cal_mode) != 0)
    {
        fits_update_key(fptr, TDOUBLE, "CAL_FREQ", &(hdr.cal_freq), NULL, &status);
        fits_update_key(fptr, TDOUBLE, "CAL_DCYC", &(hdr.cal_dcyc), NULL, &status);
        fits_update_key(fptr, TDOUBLE, "CAL_PHS", &(hdr.cal_phs), NULL, &status);
    }
    fits_update_key(fptr, TINT, "NPOL", &(hdr.npol), NULL, &status);
    fits_update_key(fptr, TINT, "NCHAN", &(hdr.nchan), NULL, &status);
    fits_update_key(fptr, TDOUBLE, "CHAN_BW", &(hdr.chan_bw), NULL, &status);
    fits_update_key(fptr, TINT, "NSUBBAND", &(hdr.nsubband), NULL, &status);
    fits_update_key(fptr, TDOUBLE, "EFSAMPFR", &(hdr.efsampfr), NULL, &status);
    fits_update_key(fptr, TDOUBLE, "FPGACLK", &(hdr.fpgaclk), NULL, &status);
    fits_update_key(fptr, TDOUBLE, "HWEXPOSR", &(hdr.hwexposr), NULL, &status);
    fits_update_key(fptr, TDOUBLE, "FILTNEP", &(hdr.filtnep), NULL, &status);
    fits_update_key(fptr, TDOUBLE, "STTMJD", &(hdr.sttmjd), NULL, &status);
    /* 计算 DATA 向量的元素数（使用 hdr.npol 而非硬编码 4） */
    long nvec = (long)hdr.nsubband * (long)hdr.nchan * (long)hdr.npol; /* num elements */

    /* 动态取得列号，避免依赖模板中固定的列序 */
    int data_col = 0, subfreq_col = 0;
    fits_get_colnum(fptr, CASEINSEN, "DATA", &data_col, &status);
    if (status) {
        fits_report_error(stderr, status);
        return status;
    }
    fits_get_colnum(fptr, CASEINSEN, "SUBFREQ", &subfreq_col, &status);
    if (status) {
        fits_report_error(stderr, status);
        return status;
    }

    /* 修改列向量长度（repeat counts）*/
    fits_modify_vector_len(fptr, data_col, nvec, &status);                 /* DATA */
    fits_modify_vector_len(fptr, subfreq_col, (long)hdr.nsubband, &status); /* SUBFREQ */

    /* 更新 TDIM<colnum>，使用 hdr.npol */
    char tdim_key[32], tdim_val[64];
    snprintf(tdim_key, sizeof(tdim_key), "TDIM%d", data_col);
    snprintf(tdim_val, sizeof(tdim_val), "(%d,%d,%d,1,1)", hdr.nchan, hdr.nsubband, hdr.npol);
    fits_update_key(fptr, TSTRING, tdim_key, tdim_val, NULL, &status);


    // Update the column sizes for the colums containing arrays
    // itmp = hdr.nsubband * hdr.nchan * hdr.npol; // num elements, not bytes
 
    // fits_modify_vector_len(fptr, 20, itmp, &status);         // DATA
    // fits_modify_vector_len(fptr, 14, hdr.nsubband, &status); // SUBFREQ

    // // Update the TDIM field for the data column
    // sprintf(ctmp, "(%d,%d,4,1,1)", hdr.nchan, hdr.nsubband);
    // fits_update_key(fptr, TSTRING, "TDIM20", ctmp, NULL, &status);

    fits_flush_file(fptr, &status);

    return status;
}

int sdfits::sdfits_write_subint()
{
    int row;
    char *temp_str;
    double temp_dbl;

    int nivals = hdr.nchan * hdr.nsubband * hdr.npol; // 4 stokes parameters
    // Create the initial file or change to a new one if needed.
    if (new_file || (multifile == 1 && rownum > rows_per_file))
    {

        if (!new_file)
        {
            printf("Closing file '%s'\n", filename);
            fits_close_file(fptr, &status);
        }
        sdfits_create();
    }
    row = rownum;
    temp_str = data_columns.object;
    temp_dbl = 0.0;
    data_columns.centre_freq_idx++;
    // data_columns.integ_num = ;
    fits_write_col(fptr, TDOUBLE, 1, row, 1, 1, &(data_columns.time), &status);
    fits_write_col(fptr, TINT, 2, row, 1, 1, &(data_columns.time_counter), &status);
    fits_write_col(fptr, TINT, 3, row, 1, 1, &(data_columns.integ_num), &status);
    fits_write_col(fptr, TFLOAT, 4, row, 1, 1, &(data_columns.exposure), &status);
    fits_write_col(fptr, TSTRING, 5, row, 1, 1, &temp_str, &status);
    fits_write_col(fptr, TFLOAT, 6, row, 1, 1, &(data_columns.azimuth), &status);
    fits_write_col(fptr, TFLOAT, 7, row, 1, 1, &(data_columns.elevation), &status);
    fits_write_col(fptr, TFLOAT, 8, row, 1, 1, &(data_columns.bmaj), &status);
    fits_write_col(fptr, TFLOAT, 9, row, 1, 1, &(data_columns.bmin), &status);
    fits_write_col(fptr, TFLOAT, 10, row, 1, 1, &(data_columns.bpa), &status);
    fits_write_col(fptr, TINT, 11, row, 1, 1, &(data_columns.accumid), &status);
    fits_write_col(fptr, TINT, 12, row, 1, 1, &(data_columns.sttspec), &status);
    fits_write_col(fptr, TINT, 13, row, 1, 1, &(data_columns.stpspec), &status);
    fits_write_col(fptr, TDOUBLE, 14, row, 1, hdr.nsubband, (data_columns.centre_freq), &status);
    fits_write_col(fptr, TFLOAT, 15, row, 1, 1, &(data_columns.centre_freq_idx), &status);
    fits_write_col(fptr, TDOUBLE, 16, row, 1, 1, &temp_dbl, &status);
    fits_write_col(fptr, TDOUBLE, 17, row, 1, 1, &(hdr.chan_bw), &status);
    fits_write_col(fptr, TDOUBLE, 18, row, 1, 1, &(data_columns.ra), &status);
    fits_write_col(fptr, TDOUBLE, 19, row, 1, 1, &(data_columns.dec), &status);
    fits_write_col(fptr, TFLOAT, 20, row, 1, nivals, data_columns.data, &status);
    fits_write_col(fptr, TINT, 21, row, 1, 1, &(data_columns.cal_on), &status);
    // fits_write_col(fptr, TDOUBLE, 22, row, 1, 1, &(data_columns.cal_phase), &status);
    
    fits_flush_file(fptr, &status);
    if (status)
        fits_report_error(stderr, status);
    // Flush the buffers if not finished with the file
    // Note:  this use is not entirely in keeping with the CFITSIO
    //        documentation recommendations.  However, manually
    //        correcting NAXIS2 and using fits_flush_buffer()
    //        caused occasional hangs (and extrememly large
    //        files due to some infinite loop).
    /*    fits_flush_file(sf->fptr, status);
        // Print status if bad
        if (*status) {
            fprintf(stderr, "Error writing subint %d:\n", sf->rownum);
            fits_report_error(stderr, *status);
            fflush(stderr);
        }
    */
    // Now update some key values if no CFITSIO errors
    if (!status)
    {
        rownum++;
        tot_rows++;
        N += 1;
        T += data_columns.exposure;
    }

    return status;
}

// Close the FITS file
int sdfits::sdfits_close()
{
    status = 0;
    if (fptr != nullptr)
    {
        // int flush_status = fits_flush_file(fptr, &status);
        int close_status = fits_close_file(fptr, &status);

        fptr = nullptr;
        if (close_status)
        {
            fits_report_error(stderr, status);
            return status; // Return error code
        }

        std::cout << "Done.  "
                  << (mode == 'r' ? "Read" : "Wrote") << " "
                  << tot_rows << " data rows (" << T << " sec) "
                  << "in " << filenum << " files "
                  << "(status = " << status << ")." << std::endl;
    }
    return 0;
}
