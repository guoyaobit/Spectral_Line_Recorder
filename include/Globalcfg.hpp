#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <unordered_set>
#include <vector>
#include <memory>
#include <thread>
#include "readerwriterqueue.h"
#include "readerwritercircularbuffer.h"
#include <ObservationId.hpp>
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
    std::vector<uint16_t> result_ports;
    int continuum_inputs = 64;
    std::string folder = "";
    std::string Observation_ID = "";
    uint32_t Observation_numeric_id = 0;
    bool observation_id_configured = false;
    std::string object = "";
    bool source_on = true;
    bool Debug_mode = false;
    const char *source_label() const
    {
        return source_on ? "ON" : "OFF";
    }
    // 初始化 YAML 配置
    bool initFromYaml(const std::string &filename)
    {
        try
        {
            YAML::Node config = YAML::LoadFile(filename);
            if (config["Debug"])
                Debug_mode = config["Debug"].as<bool>();
            if (config["observation_mode"])
                observation_mode = parseObservationMode(config["observation_mode"]);
            const YAML::Node ports = config["result_ports"];
            if (!ports || !ports.IsSequence() || ports.size() == 0)
            {
                logger_->error(
                    "result_ports must contain at least one TCP port");
                return false;
            }
            result_ports.clear();
            std::unordered_set<uint16_t> unique_ports;
            for (const auto &node : ports)
            {
                const int port = node.as<int>();
                if (port < 1 || port > 65535)
                {
                    logger_->error(
                        "Invalid result port {}; valid range is 1-65535",
                        port);
                    return false;
                }
                const auto value = static_cast<uint16_t>(port);
                if (!unique_ports.insert(value).second)
                {
                    logger_->error("Duplicate result port {}", port);
                    return false;
                }
                result_ports.push_back(value);
            }
            if (config["continuum_inputs"])
                continuum_inputs = config["continuum_inputs"].as<int>();
            if (continuum_inputs <= 0 || continuum_inputs > 64)
            {
                logger_->error(
                    "continuum_inputs must be between 1 and 64, got {}",
                    continuum_inputs);
                return false;
            }
            if (!config["Storage_folder"] ||
                !config["Storage_folder"].IsScalar())
            {
                logger_->error(
                    "Configuration must contain scalar Storage_folder");
                return false;
            }
            folder = config["Storage_folder"].as<std::string>();
            if (folder.empty())
            {
                logger_->error("Storage_folder must not be empty");
                return false;
            }
            const YAML::Node observation_id_node = config["Observation_ID"];
            observation_id_configured = false;
            Observation_numeric_id = 0;
            if (observation_id_node && !observation_id_node.IsNull())
            {
                if (!observation_id_node.IsScalar())
                {
                    logger_->error("Observation_ID must be a scalar");
                    return false;
                }
                Observation_ID = observation_id_node.as<std::string>();
                if (!Observation_ID.empty() &&
                    !observation_id_is_valid(Observation_ID))
                {
                    logger_->error(
                        "Observation_ID must contain 1-64 ASCII letters, "
                        "digits, '.', '_' or '-' and must not be '.' or '..'");
                    return false;
                }
                if (!Observation_ID.empty())
                {
                    Observation_numeric_id =
                        observation_id_numeric(Observation_ID);
                    observation_id_configured = true;
                }
            }
            if(config["object"])
                object = config["object"].as<std::string>();
            if (!config["source_on"] || !config["source_on"].IsScalar())
            {
                logger_->error(
                    "Configuration must contain scalar source_on "
                    "(true/on for source, false/off for sky background)");
                return false;
            }
            const std::string source_value =
                config["source_on"].Scalar();
            std::string normalized_source_value = source_value;
            std::transform(normalized_source_value.begin(),
                           normalized_source_value.end(),
                           normalized_source_value.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (normalized_source_value == "true" ||
                normalized_source_value == "on" ||
                normalized_source_value == "yes" ||
                normalized_source_value == "1")
            {
                source_on = true;
            }
            else if (normalized_source_value == "false" ||
                     normalized_source_value == "off" ||
                     normalized_source_value == "no" ||
                     normalized_source_value == "0")
            {
                source_on = false;
            }
            else
            {
                logger_->error(
                    "Invalid source_on value '{}'; use true/on or false/off",
                    source_value);
                return false;
            }
            if (!observation_id_configured)
            {
                Observation_ID = automatic_observation_directory_id(
                    object, source_label());
            }
            // A controller-provided ID is reused exactly. In manual mode the
            // Recorder alone generates the directory name, while processing
            // nodes and Recorder consistently use spectrum obs_id zero.
            const std::filesystem::path storage_root(folder);
            std::filesystem::create_directories(storage_root);
            const std::filesystem::path observation_folder =
                storage_root / Observation_ID;
            std::filesystem::create_directories(observation_folder);
            folder = observation_folder.string();
            if (!observation_id_configured)
                logger_->warn(
                    "Observation_ID is not configured; manual mode expects "
                    "spectrum obs_id=0 and writes to {}",
                    folder);
            else
                logger_->info(
                    "Observation_ID = {} (spectrum obs_id={}); output "
                    "directory: {}",
                    Observation_ID, Observation_numeric_id, folder);
            logger_->info(
                "Observation target state: {} ({})",
                source_label(),
                source_on ? "pointing at source" : "sky background");

        }
        catch (const YAML::Exception &e)
        {
            logger_->error("YAML configuration error: {}", e.what());
            return false;
        }
        catch (const std::exception &e)
        {
            logger_->error("Failed to initialize observation output: {}",
                           e.what());
            return false;
        }
        return true;

    }

private:
    GlobalConfig() {}
    ~GlobalConfig() = default;
};
