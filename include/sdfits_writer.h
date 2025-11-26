#pragma once
#include <vector>
#include <Globalcfg.hpp>
#include <string>
#include "fitsio.h"

class SDFITSWriter {
public:
    // 构造函数
    // filename: 输出文件名
    // n_channels: 每个通道数量
    // start_freq: 起始频率 Hz
    // chan_bw: 通道带宽 Hz
    // scan_id: 可选扫描ID
    SDFITSWriter(const std::string &filename,const spectrum_header& hdr);

    // 析构函数，关闭文件
    ~SDFITSWriter();

    // 追加一帧数据
    // timestamp_ns: 纳秒级时间戳
    // data: I/Q/U/V 顺序排列，长度 = n_channels * 4
    void append_frame(uint64_t timestamp_ns, const std::vector<float> &data);

private:
    fitsfile *fptr;
    int status;
    uint32_t n_channels;
    float start_freq;
    float stop_freq;
    float chan_bw;
    
    double ra;             // RA mid-integration
    double dec;            // DEC mid-integration
};
