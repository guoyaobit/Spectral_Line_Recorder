#pragma once

#include <vector>
#include <memory>
#include <thread>
#include "readerwriterqueue.h"
#include "readerwritercircularbuffer.h"
#include <yaml-cpp/yaml.h>
#include <iostream>
#include "spdlog/spdlog.h"
#include "spdlog/async.h"
#include "spdlog/sinks/daily_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include <spdlog/sinks/basic_file_sink.h>
#include <iomanip>
#include <sys/stat.h> 
#pragma pack(push, 1)
typedef struct {
  uint32_t magic ;//= 0x534C5231; // "SLR1"
  uint16_t version = 1;
  // UTC integration center time
  // Unix epoch nanoseconds
  uint64_t timestamp_ns;
  // observation identification
  uint32_t obs_id;         // observation ID
  uint32_t integration_id; // integration counter
  // frequency information
  uint16_t subband_id; // 子频段编号
  uint16_t window_id;  // window id
  double subband_start_freq;
  double subband_end_freq;
  double start_freq_hz;
  double channel_bw_hz;
  uint32_t n_channels;
  uint8_t stokes;
  // packet fragmentation
  uint16_t pkt_id;
  uint16_t total_pkt;
  // integration
  float exposure; // seconds
  // calibration
  uint8_t noise_state; // OFF=0 ON=1 MIX=2
  uint8_t cal_mode;    // optional
  uint16_t reserved2;

  // telescope direction
  double ra;  // rad
  double dec; // rad
  // data quality
  uint32_t flags; // overflow/dropout/etc
} spectrum_header;
#pragma pack(pop)
// struct Packet
// {
//     uint8_t payload[8192]; // 4096*(Re + Im)
// };
// struct PacketBatch
// {
//     int count;
//     std::vector<size_t> pkt_id;
//     Packet *buffer;             // 连续大 buffer
//     std::vector<Packet *> pkts; // 一次 FFT 的数据包集合
// };

// // 最终结果存储结构
// struct StokesResult
// {
//     size_t frame_id;         // 哪一帧/积累段
//     std::vector<float> data; // Nfft 个频点，每个频点一个 float4(I,Q,U,V)
// };
// struct WindowConfig
// {
//     float start_freq;
//     size_t start_idx;
//     float end_freq;
//     size_t end_idx;
//     float center_freq;
//     float BW;
//     VdifUdpSender sender;
//     int port; // udp port
// };
// struct SubbandConfig
// {
//     // int gpu_id;
//     float start_freq;
//     float end_freq;
//     static constexpr float BW = 256e6f;
//     std::vector<WindowConfig *> windows;
// };


class GlobalConfig
{
public:
    static GlobalConfig &getInstance()
    {
        static GlobalConfig instance;
        return instance;
    }
    GlobalConfig(const GlobalConfig &) = delete;
    GlobalConfig &operator=(const GlobalConfig &) = delete;
    std::shared_ptr<spdlog::logger> logger_;
    void initlog()
    {
        // 生成带时间戳的日志文件名
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm = *std::localtime(&t);

        std::ostringstream oss;
        oss << "log_" << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S") << ".log";
        std::string logfile = oss.str();

        // 初始化 spdlog 线程池（队列大小、线程数）
        spdlog::init_thread_pool(1 << 16, 1); // 64K 队列，1 个后台线程

        // 创建文件 sink
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logfile, true);

        // 创建异步 logger
        logger_ = std::make_shared<spdlog::async_logger>(
            "async_logger",
            file_sink,
            spdlog::thread_pool(),
            spdlog::async_overflow_policy::block);

        // 设置为默认 logger
        spdlog::set_default_logger(logger_);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [thread %t] [%^%l%$] %v");
        spdlog::set_level(spdlog::level::debug);
        spdlog::flush_every(std::chrono::seconds(1));
        printf("程序启动，日志文件: %s\n", logfile.c_str());
    }
    // default para
    int recv_streams = 8;
    std::string folder = "";
    std::string object = "";
    bool source_on = true;

    // const int sampling_rate = 256e6; // samaping rate
    // std::string master_node_ip;
    // const int precision = 1 + 1;  // real 8bit ,image 8bit
    // const int packet_size = 8192; // 每个数据包字节数
    // int total_nfft = 65536;       // must be multipied by 4096
    // float win_bw = 256e6;
    // int win_channels = 4096;
    // double integration_t = 1;
    int observation_mode = 1; // 默认单窗口分子谱线模式

    // int batchsize() const { return total_nfft / 4096; }
    // // 每次 FFT 的时间长度
    // double fft_period() const
    // {
    //     return static_cast<double>(total_nfft) / sampling_rate;
    // }

    // // 总积分时间，调整为 FFT 周期整数倍
    // double integration_time() const
    // {
    //     int N = static_cast<int>(integration_t / fft_period());
    //     if (N < 1)
    //         N = 1; // 至少 1 个 FFT
    //     return N * fft_period();
    // }
    // input queques
    // std::size_t QUEUE_CAPACITY = 64; // key value about memory usage
    // std::vector<moodycamel::BlockingReaderWriterCircularBuffer<PacketBatch *>> g_in_queues;
    // std::vector<std::vector<PacketBatch *>> g_in_pools;
    // std::vector<SubbandConfig> subbands;

    // 初始化 YAML 配置
    bool initFromYaml(const std::string &filename)
    {
        try
        {
            YAML::Node config = YAML::LoadFile(filename);
            // std::cout<<recv_streams<<std::endl;
            if (config["observation_mode"])
                observation_mode = config["observation_mode"].as<int>();
            if (config["recv_streams"])
                recv_streams = config["recv_streams"].as<int>();
            if (config["Storage_folder"])
                folder = config["Storage_folder"].as<std::string>();
            if(config["object"])
                object = config["object"].as<std::string>();
            if(config["source_on"])
                source_on = config["source_on"].as<bool>();
            // std::string folderName = getTimeString();
            // std::string targetPath = folder + "/" + folderName;
            // // 创建文件夹
            // if (mkdir(targetPath.c_str(), 0755) != 0)
            // {
            //     perror("mkdir failed");
            //     return 1;
            // }

            // 切换工作目录
            // if (chdir(targetPath.c_str()) != 0)
            // {
            //     perror("chdir failed");
            //     return 1;
            // }
            // if (config["master_node_ip"] && config["master_node_ip"].IsScalar())
            // {
            //     master_node_ip = config["master_node_ip"].as<std::string>();
            // }
            // else
            // {
            //     master_node_ip = "127.0.0.1"; // 默认值
            // }

            // std::cout<<recv_streams<<std::endl;
            // if (config["sampling_rate"])
            //     sampling_rate = config["sampling_rate"].as<int64_t>();
            // if (config["total_nfft"])
            //     total_nfft = config["total_nfft"].as<int>();

            // if (config["win_bw"])
            //     win_bw = config["win_bw"].as<float>();

            // std::cout<< win_bw<<std::endl;
            // if (config["win_channels"])
            //     win_channels = config["win_channels"].as<int>();
            // get total nfft from para

            // total_nfft = sampling_rate / win_bw * win_channels;
            // printf("%d\n", total_nfft);

            // if (config["queue_capacity"])
            //     QUEUE_CAPACITY = config["queue_capacity"].as<std::size_t>();

            // if (config["integration_t"])
            //     integration_t = config["integration_t"].as<double>();

            // if (!config["subbands"] || !config["subbands"].IsSequence())
            // {
            //     throw std::runtime_error("配置文件缺少 subbands config!");
            // }
            // int send_start_port = 60000;
            // for (const auto &sbNode : config["subbands"])
            // {
            //     SubbandConfig sb;
            //     // sb.gpu_id = sbNode["gpu_id"].as<int>();
            //     // sb.A_port = sbNode["A_port"].as<int>();
            //     // sb.B_port = sbNode["B_port"].as<int>();
            //     sb.start_freq = sbNode["start_freq"].as<float>();
            //     sb.end_freq = sbNode["end_freq"].as<float>();
            //     if(!sbNode["windows"])
            //     {
            //         throw std::runtime_error("配置文件缺少 window config!");
            //     }
            //     for (const auto &winNode : sbNode["windows"])
            //     {
            //         WindowConfig* w= new WindowConfig();
            //         w->center_freq = winNode["center_freq"].as<float>();
            //         w->BW = win_bw;
            //         w->start_freq = w->center_freq - win_bw / 2;
            //         if(w->start_freq < sb.start_freq)
            //         {
            //             throw std::runtime_error("Window start fre < subband start fre!");
            //         }
            //         w->end_freq = w->center_freq + win_bw / 2;
            //         if(w->end_freq>sb.end_freq)
            //         {
            //             throw std::runtime_error("Window end fre > subband end fre!");
            //         }
            //         w->start_idx = w->start_freq / sb.BW * total_nfft;
            //         w->end_idx = w->end_freq / sb.BW * total_nfft;
            //         if (w->end_idx - w->start_idx != win_channels)
            //         {
            //             logger_->error("YAML : cal start_idx or end_idx in windows error ! {} != {}", w->end_idx - w->start_idx, win_channels);
            //         }

            //         w->sender.init(master_node_ip,send_start_port);
            //         send_start_port+=1;
            //         logger_->info("dest ip {},port {}", master_node_ip,send_start_port);
            //         logger_->info("window start_idx = {} , window end_idx = {}, w->end_idx-w->start_idx = {}",
            //                       w->start_idx, w->end_idx, w->end_idx - w->start_idx);
            //         // std::cout<<w->start_idx<<" to " <<w->end_idx <<" == "<< w->end_idx-w->start_idx<<std::endl;
            //         // w->BW = winNode["BW"].as<float>();
            //         sb.windows.push_back(w);
            //     }

            //     if (sb.windows.size() > 4)
            //     {
            //         logger_->error("每个子带最多只能配置 4 个窗口!");
            //     }
            //     subbands.push_back(sb);
            // }
            // return true;
        }
        catch (const YAML::Exception &e)
        {
            logger_->error("YAML 配置解析失败: ", e.what());
            return false;
        }
        return true;

        // return checkConfig();
    }
    // bool checkConfig() const
    // {
    //     bool ok = true;
    //     if (recv_streams <= 0)
    //     {
    //         logger_->error(" recv_streams must be > 0");
    //         ok = false;
    //     }

    //     if (total_nfft <= 0)
    //     {
    //         logger_->error(" total_nfft must be > 0");
    //         ok = false;
    //     }
    //     if (total_nfft % 4096 != 0)
    //     {
    //         logger_->error(" total_nfft must be a multiple of 4096 (current value: {}", total_nfft);
    //         ok = false;
    //     }
    //     if (QUEUE_CAPACITY < 64)
    //     {
    //         logger_->error(" queue_capacity must be > 32");
    //         ok = false;
    //     }
    //     if (observation_mode < 1 || observation_mode > 3)
    //     {
    //         logger_->error(" observation_mode must be 1, 2, or 3");
    //         ok = false;
    //     }
    //     return ok;
    // }

private:
    GlobalConfig() {}
    ~GlobalConfig() = default;
};
