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
#include <filesystem>
#include <cstddef>
#pragma pack(push, 1)
typedef struct {
  uint32_t magic ;//= 0x534C5231; // "SLR1"
  uint16_t version = 2;
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
  uint8_t beam_id = 0; // 0=A, 1=B
  uint8_t reserved2 = 0;

  // telescope direction
  double ra;  // rad
  double dec; // rad
  // data quality
  uint32_t flags; // overflow/dropout/etc
} spectrum_header;
#pragma pack(pop)

static_assert(sizeof(spectrum_header) == 95,
              "spectrum_header protocol size must remain 95 bytes");
static_assert(offsetof(spectrum_header, beam_id) == 73,
              "spectrum_header beam_id offset must remain stable");
enum class ObservationMode : uint8_t {
  BASEBAND = 0, // 基带记录模式 (Raw Baseband Recording)
  SPECTRAL = 1, // 谱线观测模式 (Spectral Line Observation)
  CONTINUUM = 2 // 连续谱观测模式 (Continuum Observation)
};

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
    ObservationMode observation_mode = ObservationMode::SPECTRAL; // 默认分子谱线模式
    ObservationMode parseObservationMode(const YAML::Node &node) {
        const int mode = node.as<int>();
        switch (mode) {
        // case 0:
        // return ObservationMode::BASEBAND;
        case 1:
        return ObservationMode::SPECTRAL;
        case 2:
        return ObservationMode::CONTINUUM;
        default:
        throw std::runtime_error(
            "Invalid observation_mode: " + std::to_string(mode) +
            " (valid values: 1, 2)");
        }
    }
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
    int continuum_inputs = 64;
    std::string folder = "";
    std::string object = "";
    bool source_on = true;
    bool Debug_mode = false;
    std::string getTimeString()
    {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm localTime{};
        localtime_r(&t, &localTime);
        std::ostringstream oss;
        oss << std::put_time(&localTime, "%Y-%m-%d_%H-%M-%S");
        return oss.str();
    }
    // 初始化 YAML 配置
    bool initFromYaml(const std::string &filename)
    {
        try
        {
            YAML::Node config = YAML::LoadFile(filename);
            // std::cout<<recv_streams<<std::endl;
            if (config["Debug"])
                Debug_mode = config["Debug"].as<bool>();
            if (config["observation_mode"])
                observation_mode = parseObservationMode(config["observation_mode"]);
            if (config["recv_streams"])
                recv_streams = config["recv_streams"].as<int>();
            if (config["continuum_inputs"])
                continuum_inputs = config["continuum_inputs"].as<int>();
            if (continuum_inputs <= 0 || continuum_inputs > 64)
            {
                logger_->error(
                    "continuum_inputs must be between 1 and 64, got {}",
                    continuum_inputs);
                return false;
            }
            if (config["Storage_folder"])
                folder = config["Storage_folder"].as<std::string>();
            if(config["object"])
                object = config["object"].as<std::string>();
            if(config["source_on"])
                source_on = config["source_on"].as<bool>();
            const std::string time_string = getTimeString();

            if (observation_mode == ObservationMode::SPECTRAL)
            {
                folder += "/" +
                        config["object"].as<std::string>() + "_" +
                        config["source_on"].as<std::string>() + "_" +
                        time_string;
            }
            else if (observation_mode == ObservationMode::CONTINUUM)
            {
                folder += "/" + time_string;
            }
            std::filesystem::create_directories(folder);

        }
        catch (const YAML::Exception &e)
        {
            logger_->error("YAML 配置解析失败: ", e.what());
            return false;
        }
        return true;

    }

private:
    GlobalConfig() {}
    ~GlobalConfig() = default;
};
