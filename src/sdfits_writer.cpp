#include "sdfits_writer.h"
#include <iostream>
#include <vector>
#include <cstring>

SDFITSWriter::SDFITSWriter(const std::string &filename, const spectrum_header &hdr)
    : fptr(nullptr), status(0), n_channels(hdr.n_channels), start_freq(hdr.start_freq_hz), stop_freq(hdr.start_freq_hz + hdr.channel_bw_hz)
{
    std::string fullpath = "!" + filename; // "!" 表示覆盖已有文件

    if (fits_create_file(&fptr, fullpath.c_str(), &status))
    {
        fits_report_error(stderr, status);
        return;
    }

    // 创建空图像HDU
    fits_create_img(fptr, FLOAT_IMG, 0, NULL, &status);

    // 列格式
    std::string nchan_str = std::to_string(n_channels);
    std::string freq_form_str = nchan_str + "D"; // double array
    std::string data_form_str = nchan_str + "E"; // float array

    // 保持字符串的实际存储
    std::vector<char *> tform = {
        const_cast<char *>("1D"),
        const_cast<char *>(freq_form_str.c_str()),
        const_cast<char *>(data_form_str.c_str()),
        const_cast<char *>(data_form_str.c_str()),
        const_cast<char *>(data_form_str.c_str()),
        const_cast<char *>(data_form_str.c_str())};

    std::vector<char *> tunit = {
        const_cast<char *>("s"),
        const_cast<char *>("Hz"),
        const_cast<char *>(""),
        const_cast<char *>(""),
        const_cast<char *>(""),
        const_cast<char *>("")};

    char *ttype[] = {
        const_cast<char *>("TIME"),
        const_cast<char *>("FREQ"),
        const_cast<char *>("I"),
        const_cast<char *>("Q"),
        const_cast<char *>("U"),
        const_cast<char *>("V")};

    // 创建 BINARY TABLE
    fits_create_tbl(fptr, BINARY_TBL, 0, 6, ttype,
                    tform.data(), tunit.data(),
                    const_cast<char *>("SINGLE_DISH"), &status);
    // 写关键字
    int itmp;
    char ctmp[40];
    fits_get_system_time(ctmp, &itmp, &status);
    fits_write_key(fptr, TSTRING, "DATE", ctmp, NULL, &status);

    fits_write_key(fptr, TFLOAT, "FSTART", &start_freq, "Start frequency (Hz)", &status);
    float fstop = stop_freq;
    chan_bw = hdr.channel_bw_hz;
    fits_write_key(fptr, TFLOAT, "FSTOP", &fstop, "Stop frequency (Hz)", &status);
    fits_write_key(fptr, TFLOAT, "CHBW", &chan_bw, "Channel bandwidth (Hz)", &status);
    fits_write_key(fptr, TLONG, "NCHAN", &n_channels, "Number of channels", &status);
    fits_flush_file(fptr, &status);
}

SDFITSWriter::~SDFITSWriter()
{
    if (fptr)
    {
        fits_close_file(fptr, &status);
        if (status)
            fits_report_error(stderr, status);
    }
}

void SDFITSWriter::append_frame(uint64_t timestamp_ns, const std::vector<float> &data)
{

    if (data.size() != n_channels * 4)
    {
        std::cerr << "Error: data size mismatch, expected " << n_channels * 4 << ", got " << data.size() << std::endl;
        return;
    }

    double time_sec = timestamp_ns * 1e-9;
    
    // 构造频率轴
    std::vector<double> freqs(n_channels);
    // for (int i = 0; i < n_channels; ++i)
    //     freqs[i] = start_freq + (double)i * 256e6/n_channels;
    // printf("%f\n",freqs[0]);
    // printf("%f\n",freqs[n_channels-1]);

    // 分离 I/Q/U/V
    std::vector<float> x(n_channels);
    std::vector<float> y(n_channels);
    std::vector<float> z(n_channels);
    std::vector<float> w(n_channels);

    for (long i = 0; i < n_channels; ++i)
    {
        x[i] = data[4 * i + 0];
        y[i] = data[4 * i + 1];
        z[i] = data[4 * i + 2];
        w[i] = data[4 * i + 3];
    }
    float max = 0;
    float max_freq = 0;
    int max_idx = 0;
    for (int i = 0; i < n_channels; i++)
    {
        if (x[i]> max)
        {
            max = x[i];
            // max_freq = freqs[i];
            max_idx = i;
        }
    }
    printf("max_idx:%d,%f\n",max_idx,(double)max_idx*256e6/n_channels);
    long nrow;
    fits_get_num_rows(fptr, &nrow, &status);
    long row = nrow + 1;
    time_sec = std::chrono::duration_cast<std::chrono::duration<double>>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    fits_write_col(fptr, TDOUBLE, 1, row, 1, 1, &time_sec, &status);
    fits_write_col(fptr, TDOUBLE, 2, row, 1, n_channels, freqs.data(), &status);
    fits_write_col(fptr, TFLOAT, 3, row, 1, n_channels, x.data(), &status);
    fits_write_col(fptr, TFLOAT, 4, row, 1, n_channels, y.data(), &status);
    fits_write_col(fptr, TFLOAT, 5, row, 1, n_channels, z.data(), &status);
    fits_write_col(fptr, TFLOAT, 6, row, 1, n_channels, w.data(), &status);
    
    if (status)
        fits_report_error(stderr, status);
    fits_flush_file(fptr, &status);
}
