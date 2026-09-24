#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ContinuumFits.h>
#include <Globalcfg.hpp>
#include <SpectrumTransport.hpp>
#include <sdfits.h>
#include <time_utils.h>

#include <zmq.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace {

constexpr uint32_t SPECTRUM_MAGIC = 0x534C5231U;
constexpr uint16_t SPECTRUM_VERSION_V2 = 2;
constexpr uint16_t MAX_GLOBAL_SUBBAND_ID = 31;
constexpr uint32_t MAX_SPECTRUM_CHANNELS = 65536U * 256U;
constexpr uint16_t SUBBANDS_PER_SERVER = 4;
constexpr size_t SPECTRUM_WRITER_COUNT = 8;
constexpr size_t SPECTRUM_WRITE_QUEUE_BYTES = 256ULL * 1024ULL * 1024ULL;
constexpr size_t CONTINUUM_QUEUE_CAPACITY = 8192;
constexpr size_t MAX_ZMQ_IO_THREADS = 8;

auto &cfg = GlobalConfig::getInstance();
ContinuumFits continuum_fits;

size_t zmq_io_thread_count() {
  return std::min(MAX_ZMQ_IO_THREADS,
                  std::max<size_t>(1, cfg.result_ports.size()));
}

#ifdef __linux__
const std::vector<int> &allowed_cpus() {
  static const std::vector<int> cpus = [] {
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    std::vector<int> result;
    if (sched_getaffinity(0, sizeof(affinity), &affinity) != 0)
      return result;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
      if (CPU_ISSET(cpu, &affinity))
        result.push_back(cpu);
    }
    return result;
  }();
  return cpus;
}

int cpu_for_slot(size_t slot) {
  const auto &cpus = allowed_cpus();
  return cpus.empty() ? -1 : cpus[slot % cpus.size()];
}

void bind_current_thread(size_t slot, const std::string &name) {
  const int cpu = cpu_for_slot(slot);
  if (cpu < 0) {
    cfg.logger_->warn("Cannot bind {}: process CPU affinity is empty", name);
    return;
  }

  const std::string thread_name = name.substr(0, 15);
  pthread_setname_np(pthread_self(), thread_name.c_str());

  cpu_set_t affinity;
  CPU_ZERO(&affinity);
  CPU_SET(cpu, &affinity);
  const int error =
      pthread_setaffinity_np(pthread_self(), sizeof(affinity), &affinity);
  if (error != 0) {
    cfg.logger_->warn("Failed to bind {} to CPU {}: {}", name, cpu,
                      std::strerror(error));
    return;
  }
  cfg.logger_->info("Bound {} to CPU {}", name, cpu);
}

std::string cpu_slot_list(size_t first_slot, size_t count) {
  std::ostringstream stream;
  for (size_t i = 0; i < count; ++i) {
    if (i != 0)
      stream << ',';
    stream << cpu_for_slot(first_slot + i);
  }
  return stream.str();
}
#else
const std::vector<int> &allowed_cpus() {
  static const std::vector<int> cpus;
  return cpus;
}
int cpu_for_slot(size_t) { return -1; }
void bind_current_thread(size_t, const std::string &) {}
std::string cpu_slot_list(size_t, size_t) { return "unsupported"; }
#endif

size_t receiver_cpu_slot(size_t receiver_index) {
  return 1 + zmq_io_thread_count() + receiver_index;
}

size_t writer_cpu_slot(size_t writer_index) {
  return 1 + zmq_io_thread_count() +
         cfg.result_ports.size() + writer_index;
}

class ZmqMessage {
public:
  ZmqMessage() : valid_(zmq_msg_init(&message_) == 0) {}

  ~ZmqMessage() {
    if (valid_)
      zmq_msg_close(&message_);
  }

  ZmqMessage(const ZmqMessage &) = delete;
  ZmqMessage &operator=(const ZmqMessage &) = delete;

  ZmqMessage(ZmqMessage &&other) noexcept
      : valid_(zmq_msg_init(&message_) == 0) {
    if (valid_ && other.valid_)
      zmq_msg_move(&message_, &other.message_);
  }

  ZmqMessage &operator=(ZmqMessage &&other) noexcept {
    if (this == &other)
      return *this;
    if (valid_)
      zmq_msg_close(&message_);
    valid_ = zmq_msg_init(&message_) == 0;
    if (valid_ && other.valid_)
      zmq_msg_move(&message_, &other.message_);
    return *this;
  }

  int receive(void *socket) {
    return valid_ ? zmq_msg_recv(&message_, socket, 0) : -1;
  }

  size_t size() const {
    return valid_ ? zmq_msg_size(const_cast<zmq_msg_t *>(&message_)) : 0;
  }

  const uint8_t *data() const {
    return valid_ ? static_cast<const uint8_t *>(
                        zmq_msg_data(const_cast<zmq_msg_t *>(&message_)))
                  : nullptr;
  }

private:
  zmq_msg_t message_{};
  bool valid_ = false;
};

struct BandKey {
  uint16_t subband_id;
  uint16_t window_id;
  uint8_t beam_id;
  double f_start;
  double f_stop;
  double channel_bw_hz;
  uint32_t n_channels;

  bool operator==(const BandKey &other) const noexcept {
    return subband_id == other.subband_id && window_id == other.window_id &&
           beam_id == other.beam_id && f_start == other.f_start &&
           f_stop == other.f_stop &&
           channel_bw_hz == other.channel_bw_hz &&
           n_channels == other.n_channels;
  }
};

struct BandKeyHash {
  size_t operator()(const BandKey &key) const noexcept {
    size_t hash = std::hash<uint16_t>{}(key.subband_id);
    const auto combine = [&hash](size_t value) {
      hash ^= value + 0x9e3779b9U + (hash << 6) + (hash >> 2);
    };
    combine(std::hash<uint16_t>{}(key.window_id));
    combine(std::hash<uint8_t>{}(key.beam_id));
    combine(std::hash<double>{}(key.f_start));
    combine(std::hash<double>{}(key.f_stop));
    combine(std::hash<double>{}(key.channel_bw_hz));
    combine(std::hash<uint32_t>{}(key.n_channels));
    return hash;
  }
};

struct WriterState {
  sdfits writer;
  std::mutex mutex;
};

std::unordered_map<BandKey, std::shared_ptr<WriterState>, BandKeyHash> writers;
std::mutex writers_map_mutex;
std::mutex cfitsio_mutex;

struct CompletedSpectrum {
  spectrum_header header;
  uint16_t server_id;
  uint64_t sequence;
  ZmqMessage message;
  size_t payload_offset;
  size_t payload_bytes;

  CompletedSpectrum(spectrum_header result_header, uint16_t result_server_id,
                    uint64_t result_sequence, ZmqMessage &&result_message,
                    size_t result_payload_offset,
                    size_t result_payload_bytes)
      : header(result_header), server_id(result_server_id),
        sequence(result_sequence), message(std::move(result_message)),
        payload_offset(result_payload_offset),
        payload_bytes(result_payload_bytes) {}

  const float *data() const {
    return reinterpret_cast<const float *>(message.data() + payload_offset);
  }
};

struct SpectrumWriteQueue {
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<std::unique_ptr<CompletedSpectrum>> jobs;
  size_t queued_bytes = 0;
};

std::array<SpectrumWriteQueue, SPECTRUM_WRITER_COUNT> spectrum_write_queues;

bool cfitsio_supports_parallel_io() {
  static const bool supported = fits_is_reentrant() != 0;
  return supported;
}

size_t spectrum_writer_index(const spectrum_header &header) {
  size_t identity = static_cast<size_t>(header.subband_id) * 1315423911U;
  identity ^= static_cast<size_t>(header.window_id) * 2654435761U;
  identity ^= static_cast<size_t>(header.beam_id) * 2246822519U;
  return identity % SPECTRUM_WRITER_COUNT;
}

void write_spectrum_frame(const CompletedSpectrum &result) {
  const spectrum_header &header = result.header;
  const double f_start = header.start_freq_hz;
  const double f_stop =
      f_start + header.channel_bw_hz * header.n_channels;
  const BandKey file_key{header.subband_id, header.window_id, header.beam_id,
                         f_start, f_stop, header.channel_bw_hz,
                         header.n_channels};

  std::shared_ptr<WriterState> writer_state;
  {
    std::lock_guard<std::mutex> map_lock(writers_map_mutex);
    auto writer_it = writers.find(file_key);
    if (writer_it == writers.end()) {
      writer_state = std::make_shared<WriterState>();
      sdfits &writer = writer_state->writer;
      writer.new_file = 1;

      const char beam_name = header.beam_id == 0 ? 'A' : 'B';
      const int filename_length = snprintf(
          writer.basefilename, sizeof(writer.basefilename),
          "%s/sb%02u_w%u_%.2f-%.2fMHz_%c_%s_%u", cfg.folder.c_str(),
          static_cast<unsigned>(header.subband_id),
          static_cast<unsigned>(header.window_id), f_start / 1e6,
          f_stop / 1e6, beam_name, cfg.source_label(),
          static_cast<unsigned>(header.n_channels));
      if (filename_length < 0 ||
          static_cast<size_t>(filename_length) >= sizeof(writer.basefilename)) {
        cfg.logger_->error(
            "SDFITS base filename is too long for subband {}, window {}",
            header.subband_id, header.window_id);
        return;
      }

      writer.hdr.nchan = header.n_channels;
      const uint64_t start_ns = spectrum_time::integration_start_ns(
          header.timestamp_ns, header.exposure);
      if (!spectrum_time::format_fits_utc(start_ns, writer.hdr.date_obs,
                                          sizeof(writer.hdr.date_obs))) {
        cfg.logger_->error("Failed to format DATE-OBS for timestamp {}",
                           start_ns);
        return;
      }
      writer.hdr.sttmjd = spectrum_time::unix_ns_to_mjd(start_ns);
      writer.hdr.hwexposr = header.exposure;
      writer.hdr.chan_bw = header.channel_bw_hz;
      writer.hdr.obsfreq = header.start_freq_hz +
                           header.channel_bw_hz * header.n_channels / 2;
      writer.hdr.nsubband = 1;
      writer.hdr.npol = 4;

      std::unique_lock<std::mutex> cfitsio_lock(cfitsio_mutex,
                                                std::defer_lock);
      if (!cfitsio_supports_parallel_io())
        cfitsio_lock.lock();
      const int create_status = writer.sdfits_create();
      if (create_status != 0) {
        cfg.logger_->error(
            "Failed to create SDFITS file for subband {}, window {}, beam "
            "{}, status={}",
            header.subband_id, header.window_id, beam_name, create_status);
        return;
      }
      writers.emplace(file_key, writer_state);
      cfg.logger_->info(
          "Created SDFITS output for server {}, subband {}, window {}, beam "
          "{}: {}",
          result.server_id, header.subband_id, header.window_id, beam_name,
          writer.filename);
    } else {
      writer_state = writer_it->second;
    }
  }

  std::lock_guard<std::mutex> writer_lock(writer_state->mutex);
  std::unique_lock<std::mutex> cfitsio_lock(cfitsio_mutex, std::defer_lock);
  if (!cfitsio_supports_parallel_io())
    cfitsio_lock.lock();

  sdfits &writer = writer_state->writer;
  writer.data_columns.data = reinterpret_cast<unsigned char *>(
      const_cast<float *>(result.data()));
  writer.data_columns.cal_on = header.noise_state;
  writer.data_columns.integ_num = static_cast<int>(header.integration_id);
  writer.data_columns.centre_freq[0] = writer.hdr.obsfreq;
  writer.data_columns.time = spectrum_time::unix_ns_to_mjd(
      spectrum_time::integration_start_ns(header.timestamp_ns,
                                          header.exposure));
  writer.data_columns.exposure = header.exposure;
  const int write_status = writer.sdfits_write_subint();
  if (write_status != 0) {
    cfg.logger_->error(
        "Failed to write SDFITS row to {}, server={}, sequence={}, status={}",
        writer.filename, result.server_id, result.sequence, write_status);
  }
  writer.data_columns.data = nullptr;
}

bool enqueue_spectrum_write(spectrum_header header, uint16_t server_id,
                            uint64_t sequence, ZmqMessage &&message,
                            size_t payload_offset, size_t payload_bytes) {
  const size_t index = spectrum_writer_index(header);
  SpectrumWriteQueue &queue = spectrum_write_queues[index];
  {
    std::lock_guard<std::mutex> lock(queue.mutex);
    if (!queue.jobs.empty() &&
        queue.queued_bytes + payload_bytes > SPECTRUM_WRITE_QUEUE_BYTES) {
      static std::atomic<uint64_t> dropped{0};
      const uint64_t count = dropped.fetch_add(1) + 1;
      if (count == 1 || (count & (count - 1)) == 0) {
        cfg.logger_->error(
            "Dropped complete spectrum because Writer queue {} is full: "
            "server={}, subband={}, window={}, beam={}, sequence={}, "
            "total_drops={}",
            index, server_id, header.subband_id, header.window_id,
            header.beam_id, sequence, count);
      }
      return false;
    }
    queue.queued_bytes += payload_bytes;
    queue.jobs.emplace_back(new CompletedSpectrum(
        header, server_id, sequence, std::move(message), payload_offset,
        payload_bytes));
  }
  queue.ready.notify_one();
  return true;
}

void spectrum_writer_worker(size_t index) {
  bind_current_thread(writer_cpu_slot(index),
                      "sdfits-" + std::to_string(index));
  SpectrumWriteQueue &queue = spectrum_write_queues[index];
  cfg.logger_->info("Spectrum Writer {} started", index);
  while (true) {
    std::unique_ptr<CompletedSpectrum> job;
    {
      std::unique_lock<std::mutex> lock(queue.mutex);
      queue.ready.wait(lock, [&queue] { return !queue.jobs.empty(); });
      job = std::move(queue.jobs.front());
      queue.jobs.pop_front();
      queue.queued_bytes -= job->payload_bytes;
    }
    write_spectrum_frame(*job);
  }
}

struct ContinuumResult {
  uint64_t timestamp_ns;
  uint32_t integration_id;
  uint16_t subband_id;
  uint8_t beam_id;
  float power;
  float exposure;
  uint8_t noise_state;
};

struct ContinuumQueue {
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<ContinuumResult> jobs;
} continuum_queue;

struct ContinuumFrame {
  uint64_t timestamp_ns = 0;
  double power = 0.0;
  uint32_t received_inputs = 0;
  std::unordered_set<uint32_t> inputs;
  float exposure = 0.0;
  uint8_t noise_state = 0;
  std::chrono::steady_clock::time_point created_at =
      std::chrono::steady_clock::now();
};

std::map<uint64_t, ContinuumFrame> continuum_map;
constexpr uint64_t CONTINUUM_TIME_TOLERANCE_NS = 1000;
constexpr auto CONTINUUM_FRAME_TIMEOUT = std::chrono::seconds(5);
auto last_continuum_cleanup = std::chrono::steady_clock::now();

std::map<uint64_t, ContinuumFrame>::iterator
find_continuum_frame(uint64_t timestamp_ns) {
  auto it = continuum_map.lower_bound(timestamp_ns);
  if (it != continuum_map.end()) {
    const uint64_t difference = it->first >= timestamp_ns
                                    ? it->first - timestamp_ns
                                    : timestamp_ns - it->first;
    if (difference <= CONTINUUM_TIME_TOLERANCE_NS)
      return it;
  }
  if (it != continuum_map.begin()) {
    auto previous = std::prev(it);
    const uint64_t difference = previous->first >= timestamp_ns
                                    ? previous->first - timestamp_ns
                                    : timestamp_ns - previous->first;
    if (difference <= CONTINUUM_TIME_TOLERANCE_NS)
      return previous;
  }
  return continuum_map.end();
}

void accumulate_continuum(const ContinuumResult &result) {
  const uint32_t input_id =
      (static_cast<uint32_t>(result.subband_id) << 1) | result.beam_id;
  auto it = find_continuum_frame(result.timestamp_ns);
  if (it == continuum_map.end()) {
    ContinuumFrame frame;
    frame.timestamp_ns = result.timestamp_ns;
    frame.exposure = result.exposure;
    frame.noise_state = result.noise_state;
    it = continuum_map.emplace(result.timestamp_ns, std::move(frame)).first;
  }

  ContinuumFrame &frame = it->second;
  if (frame.noise_state != result.noise_state ||
      std::abs(frame.exposure - result.exposure) > 1.0e-6f) {
    cfg.logger_->error(
        "Discarded inconsistent continuum integration: timestamp={}, "
        "first exposure/state={}/{}, received exposure/state={}/{}",
        result.timestamp_ns, frame.exposure, frame.noise_state,
        result.exposure, result.noise_state);
    continuum_map.erase(it);
    return;
  }
  if (!frame.inputs.insert(input_id).second) {
    cfg.logger_->warn(
        "Duplicate continuum result: timestamp={}, subband={}, beam={}, "
        "integration={}",
        result.timestamp_ns, result.subband_id, result.beam_id,
        result.integration_id);
    return;
  }

  frame.power += result.power;
  ++frame.received_inputs;
  if (frame.received_inputs == static_cast<uint32_t>(cfg.continuum_inputs)) {
    if (!continuum_fits.write(frame.timestamp_ns, frame.power, frame.exposure,
                              frame.noise_state)) {
      cfg.logger_->error(
          "Failed to write complete continuum integration: timestamp={}, "
          "inputs={}",
          frame.timestamp_ns, frame.received_inputs);
    }
    continuum_map.erase(it);
  }
}

void discard_expired_continuum_frames() {
  const auto now = std::chrono::steady_clock::now();
  if (now - last_continuum_cleanup < std::chrono::seconds(1))
    return;
  for (auto it = continuum_map.begin(); it != continuum_map.end();) {
    if (now - it->second.created_at >= CONTINUUM_FRAME_TIMEOUT) {
      cfg.logger_->warn(
          "Discarded incomplete continuum integration: timestamp={}, "
          "received={}/{}, total_power={}",
          it->second.timestamp_ns, it->second.received_inputs,
          cfg.continuum_inputs, it->second.power);
      it = continuum_map.erase(it);
    } else {
      ++it;
    }
  }
  last_continuum_cleanup = now;
}

bool enqueue_continuum(ContinuumResult result) {
  {
    std::lock_guard<std::mutex> lock(continuum_queue.mutex);
    if (continuum_queue.jobs.size() >= CONTINUUM_QUEUE_CAPACITY) {
      static std::atomic<uint64_t> dropped{0};
      const uint64_t count = dropped.fetch_add(1) + 1;
      if (count == 1 || (count & (count - 1)) == 0)
        cfg.logger_->error(
            "Dropped continuum result because aggregation queue is full; "
            "total_drops={}",
            count);
      return false;
    }
    continuum_queue.jobs.emplace_back(result);
  }
  continuum_queue.ready.notify_one();
  return true;
}

void continuum_worker() {
  bind_current_thread(writer_cpu_slot(0), "continuum");
  const std::string filename =
      cfg.folder + "/continuum_" + cfg.source_label() + ".fits";
  if (!continuum_fits.create(filename, 0)) {
    cfg.logger_->critical("Failed to create continuum FITS: {}", filename);
    return;
  }

  cfg.logger_->info(
      "Continuum Writer started; expecting {} subband/beam result(s) per "
      "integration",
      cfg.continuum_inputs);
  while (true) {
    ContinuumResult result{};
    bool have_result = false;
    {
      std::unique_lock<std::mutex> lock(continuum_queue.mutex);
      continuum_queue.ready.wait_for(lock, std::chrono::seconds(1), [] {
        return !continuum_queue.jobs.empty();
      });
      if (!continuum_queue.jobs.empty()) {
        result = continuum_queue.jobs.front();
        continuum_queue.jobs.pop_front();
        have_result = true;
      }
    }
    if (have_result)
      accumulate_continuum(result);
    discard_expired_continuum_frames();
  }
}

struct SequenceKey {
  uint16_t server_id;
  uint16_t subband_id;
  uint16_t window_id;
  uint8_t beam_id;

  bool operator==(const SequenceKey &other) const noexcept {
    return server_id == other.server_id && subband_id == other.subband_id &&
           window_id == other.window_id && beam_id == other.beam_id;
  }
};

struct SequenceKeyHash {
  size_t operator()(const SequenceKey &key) const noexcept {
    return (static_cast<size_t>(key.server_id) << 40) ^
           (static_cast<size_t>(key.subband_id) << 24) ^
           (static_cast<size_t>(key.window_id) << 8) ^ key.beam_id;
  }
};

struct SequenceState {
  uint64_t sequence;
  uint64_t timestamp_ns;
};

struct ReceiverStartup {
  std::mutex mutex;
  std::condition_variable ready;
  size_t completed = 0;
  bool failed = false;
};

bool validate_header(const spectrum_header &header, size_t payload_bytes) {
  if (header.magic != SPECTRUM_MAGIC ||
      header.version != SPECTRUM_VERSION_V2 || header.pkt_id != 0 ||
      header.total_pkt != 1 || header.subband_id > MAX_GLOBAL_SUBBAND_ID ||
      header.beam_id > 1 || header.n_channels == 0 ||
      header.n_channels > MAX_SPECTRUM_CHANNELS ||
      !std::isfinite(header.start_freq_hz) ||
      !std::isfinite(header.channel_bw_hz) || header.channel_bw_hz <= 0.0 ||
      !std::isfinite(header.exposure) || header.exposure <= 0.0f)
    return false;

  if (cfg.observation_mode == ObservationMode::CONTINUUM)
    return header.window_id == 0 && payload_bytes == sizeof(float);

  const size_t expected =
      static_cast<size_t>(header.n_channels) * 4 * sizeof(float);
  return payload_bytes == expected;
}

void report_receiver_startup(ReceiverStartup &startup, bool success) {
  {
    std::lock_guard<std::mutex> lock(startup.mutex);
    ++startup.completed;
    startup.failed = startup.failed || !success;
  }
  startup.ready.notify_one();
}

void receive_worker(void *context, size_t receiver_index, uint16_t port,
                    ReceiverStartup &startup) {
  bind_current_thread(receiver_cpu_slot(receiver_index),
                      "zmq-rx-" + std::to_string(receiver_index));
  void *socket = zmq_socket(context, ZMQ_PULL);
  if (socket == nullptr) {
    cfg.logger_->critical("Failed to create ZeroMQ PULL socket: {}",
                          zmq_strerror(zmq_errno()));
    report_receiver_startup(startup, false);
    return;
  }

  const int linger = 0;
  const int receive_hwm = 64;
  const int receive_buffer_bytes = 16 * 1024 * 1024;
  const int tcp_keepalive = 1;
  const int heartbeat_ms = 1000;
  const int heartbeat_timeout_ms = 3000;
  const uint64_t io_affinity =
      1ULL << (receiver_index % zmq_io_thread_count());
  const int64_t maximum_message_bytes =
      static_cast<int64_t>(sizeof(spectrum_transport_header) +
                           sizeof(spectrum_header)) +
      static_cast<int64_t>(MAX_SPECTRUM_CHANNELS) * 4 * sizeof(float);
  if (zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(linger)) != 0 ||
      zmq_setsockopt(socket, ZMQ_AFFINITY, &io_affinity,
                     sizeof(io_affinity)) != 0 ||
      zmq_setsockopt(socket, ZMQ_RCVHWM, &receive_hwm,
                     sizeof(receive_hwm)) != 0 ||
      zmq_setsockopt(socket, ZMQ_RCVBUF, &receive_buffer_bytes,
                     sizeof(receive_buffer_bytes)) != 0 ||
      zmq_setsockopt(socket, ZMQ_TCP_KEEPALIVE, &tcp_keepalive,
                     sizeof(tcp_keepalive)) != 0 ||
      zmq_setsockopt(socket, ZMQ_HEARTBEAT_IVL, &heartbeat_ms,
                     sizeof(heartbeat_ms)) != 0 ||
      zmq_setsockopt(socket, ZMQ_HEARTBEAT_TIMEOUT,
                     &heartbeat_timeout_ms,
                     sizeof(heartbeat_timeout_ms)) != 0 ||
      zmq_setsockopt(socket, ZMQ_MAXMSGSIZE, &maximum_message_bytes,
                     sizeof(maximum_message_bytes)) != 0) {
    cfg.logger_->critical("Failed to configure ZeroMQ PULL socket: {}",
                          zmq_strerror(zmq_errno()));
    zmq_close(socket);
    report_receiver_startup(startup, false);
    return;
  }

  const std::string endpoint = "tcp://*:" + std::to_string(port);
  if (zmq_bind(socket, endpoint.c_str()) != 0) {
    cfg.logger_->critical("Failed to bind {}: {}", endpoint,
                          zmq_strerror(zmq_errno()));
    zmq_close(socket);
    report_receiver_startup(startup, false);
    return;
  }
  report_receiver_startup(startup, true);

  std::unordered_map<SequenceKey, SequenceState, SequenceKeyHash> sequences;
  uint64_t sequence_gap_events = 0;
  uint64_t duplicate_events = 0;
  cfg.logger_->info("ZeroMQ receiver started on tcp://*:{}", port);

  while (true) {
    ZmqMessage message;
    const int received = message.receive(socket);
    if (received < 0) {
      const int error = zmq_errno();
      if (error == ETERM)
        break;
      if (error != EINTR)
        cfg.logger_->error("ZeroMQ receive failed on port {}: {}", port,
                           zmq_strerror(error));
      continue;
    }

    const size_t message_size = message.size();
    const uint8_t *message_data = message.data();
    if (message_size < sizeof(spectrum_transport_header) +
                           sizeof(spectrum_header)) {
      cfg.logger_->warn("Dropped undersized ZeroMQ result on port {}: {} bytes",
                        port, message_size);
      continue;
    }

    spectrum_transport_header transport{};
    std::memcpy(&transport, message_data, sizeof(transport));
    const uint8_t *body = message_data + sizeof(transport);
    const size_t body_bytes = message_size - sizeof(transport);
    if (transport.magic != SPECTRUM_TRANSPORT_MAGIC ||
        transport.version != SPECTRUM_TRANSPORT_VERSION ||
        transport.server_id > 7 || transport.message_bytes != body_bytes ||
        spectrum_crc32c(body, body_bytes) != transport.crc32c) {
      static std::atomic<uint64_t> invalid_transport{0};
      const uint64_t count = invalid_transport.fetch_add(1) + 1;
      if (count == 1 || (count & (count - 1)) == 0)
        cfg.logger_->warn(
            "Dropped invalid ZeroMQ transport message on port {}; "
            "server={}, sequence={}, total_invalid={}",
            port, transport.server_id, transport.sequence, count);
      continue;
    }

    spectrum_header header{};
    std::memcpy(&header, body, sizeof(header));
    const uint8_t *payload = body + sizeof(header);
    const size_t payload_bytes = body_bytes - sizeof(header);
    if (!validate_header(header, payload_bytes) ||
        header.subband_id / SUBBANDS_PER_SERVER != transport.server_id) {
      cfg.logger_->warn(
          "Dropped invalid spectrum message: server={}, sequence={}, "
          "subband={}, window={}, beam={}, payload_bytes={}",
          transport.server_id, transport.sequence, header.subband_id,
          header.window_id, header.beam_id, payload_bytes);
      continue;
    }

    const SequenceKey sequence_key{transport.server_id, header.subband_id,
                                   header.window_id, header.beam_id};
    auto sequence_it = sequences.find(sequence_key);
    if (sequence_it != sequences.end()) {
      const SequenceState previous = sequence_it->second;
      if (transport.sequence > previous.sequence + 1) {
        ++sequence_gap_events;
        if ((sequence_gap_events & (sequence_gap_events - 1)) == 0) {
          cfg.logger_->warn(
              "ZeroMQ result gap event {} on port {}: server={}, "
              "subband={}, window={}, beam={}, expected={}, received={}, "
              "missing={}",
              sequence_gap_events, port, transport.server_id,
              header.subband_id, header.window_id, header.beam_id,
              previous.sequence + 1, transport.sequence,
              transport.sequence - previous.sequence - 1);
        }
      } else if (transport.sequence <= previous.sequence &&
                 header.timestamp_ns <= previous.timestamp_ns) {
        ++duplicate_events;
        if ((duplicate_events & (duplicate_events - 1)) == 0) {
          cfg.logger_->warn(
              "Dropped duplicate/out-of-order ZeroMQ result event {}: "
              "server={}, subband={}, window={}, beam={}, sequence={}",
              duplicate_events, transport.server_id, header.subband_id,
              header.window_id, header.beam_id, transport.sequence);
        }
        continue;
      }
    }
    sequences[sequence_key] =
        SequenceState{transport.sequence, header.timestamp_ns};

    if (cfg.observation_mode == ObservationMode::CONTINUUM) {
      float power = 0.0f;
      std::memcpy(&power, payload, sizeof(power));
      if (!std::isfinite(power)) {
        cfg.logger_->warn(
            "Dropped non-finite continuum result: server={}, subband={}, "
            "beam={}, sequence={}",
            transport.server_id, header.subband_id, header.beam_id,
            transport.sequence);
        continue;
      }
      enqueue_continuum(ContinuumResult{
          header.timestamp_ns, header.integration_id, header.subband_id,
          header.beam_id, power, header.exposure, header.noise_state});
    } else {
      const size_t payload_offset =
          sizeof(spectrum_transport_header) + sizeof(spectrum_header);
      enqueue_spectrum_write(header, transport.server_id, transport.sequence,
                             std::move(message), payload_offset,
                             payload_bytes);
    }
  }
  zmq_close(socket);
}

} // namespace

void initialize_recorder_cpu_affinity() {
#ifdef __linux__
  // Run before the asynchronous logger is created. Its worker then inherits
  // the main-core binding, while data-path threads are reassigned explicitly
  // after startup. allowed_cpus() is evaluated first so the original cgroup or
  // systemd CPU mask remains available for the complete mapping.
  const auto &cpus = allowed_cpus();
  if (cpus.empty())
    return;
  cpu_set_t affinity;
  CPU_ZERO(&affinity);
  CPU_SET(cpus.front(), &affinity);
  pthread_setaffinity_np(pthread_self(), sizeof(affinity), &affinity);
#endif
}

int receive_results() {
  if (cfg.result_ports.empty()) {
    cfg.logger_->critical("result_ports must contain at least one TCP port");
    return -1;
  }

  std::ostringstream configured_ports;
  for (size_t i = 0; i < cfg.result_ports.size(); ++i) {
    if (i != 0)
      configured_ports << ", ";
    configured_ports << cfg.result_ports[i];
  }
  cfg.logger_->info(
      "ZeroMQ result listeners: [{}]. Port numbers do not identify "
      "subbands, beams, or windows; those identities come from the header",
      configured_ports.str());

  bind_current_thread(0, "recorder-main");
  const size_t io_threads = zmq_io_thread_count();
  const size_t writer_threads =
      cfg.observation_mode == ObservationMode::SPECTRAL
          ? SPECTRUM_WRITER_COUNT
          : 1;
  const size_t required_cpus =
      1 + io_threads + cfg.result_ports.size() + writer_threads;
  if (!allowed_cpus().empty()) {
    if (allowed_cpus().size() < required_cpus) {
      cfg.logger_->warn(
          "Recorder has {} allowed CPU(s), but {} are needed for unique "
          "main/ZeroMQ/receiver/Writer bindings; CPU assignments will wrap",
          allowed_cpus().size(), required_cpus);
    }
    cfg.logger_->info(
        "Recorder CPU map: main={}, ZeroMQ I/O=[{}], receivers=[{}], "
        "Writers=[{}]",
        cpu_for_slot(0), cpu_slot_list(1, io_threads),
        cpu_slot_list(1 + io_threads, cfg.result_ports.size()),
        cpu_slot_list(writer_cpu_slot(0), writer_threads));
  }

  if (cfg.observation_mode == ObservationMode::SPECTRAL) {
    for (size_t i = 0; i < SPECTRUM_WRITER_COUNT; ++i)
      std::thread(spectrum_writer_worker, i).detach();
  } else {
    std::thread(continuum_worker).detach();
  }

  if (cfitsio_supports_parallel_io())
    cfg.logger_->info(
        "CFITSIO is reentrant; separate output files write concurrently");
  else
    cfg.logger_->warn(
        "CFITSIO is not reentrant; output writes remain serialized");

  void *context = zmq_ctx_new();
  if (context == nullptr) {
    cfg.logger_->critical("Failed to create ZeroMQ context: {}",
                          zmq_strerror(zmq_errno()));
    return -1;
  }
  // Use one ZeroMQ I/O thread per result port, up to eight. ZMQ_AFFINITY on
  // each PULL socket maps the endpoint to one member of this I/O pool.
  if (zmq_ctx_set(context, ZMQ_IO_THREADS,
                  static_cast<int>(io_threads)) != 0) {
    cfg.logger_->critical("Failed to configure ZeroMQ I/O threads: {}",
                          zmq_strerror(zmq_errno()));
    zmq_ctx_term(context);
    return -1;
  }
#ifdef ZMQ_THREAD_AFFINITY_CPU_ADD
  std::unordered_set<int> io_cpus;
  for (size_t i = 0; i < io_threads; ++i) {
    const int cpu = cpu_for_slot(1 + i);
    if (cpu >= 0)
      io_cpus.insert(cpu);
  }
  for (const int cpu : io_cpus) {
    if (zmq_ctx_set(context, ZMQ_THREAD_AFFINITY_CPU_ADD, cpu) != 0) {
      cfg.logger_->critical("Failed to bind ZeroMQ I/O pool to CPU {}: {}",
                            cpu, zmq_strerror(zmq_errno()));
      zmq_ctx_term(context);
      return -1;
    }
  }
#else
  cfg.logger_->warn(
      "This libzmq lacks ZMQ_THREAD_AFFINITY_CPU_ADD; application threads "
      "are pinned, but the ZeroMQ I/O pool cannot be CPU-bound");
#endif

  ReceiverStartup startup;
  std::vector<std::thread> receivers;
  receivers.reserve(cfg.result_ports.size());
  for (size_t i = 0; i < cfg.result_ports.size(); ++i) {
    receivers.emplace_back(receive_worker, context, i, cfg.result_ports[i],
                           std::ref(startup));
  }

  {
    std::unique_lock<std::mutex> lock(startup.mutex);
    startup.ready.wait(lock, [&startup] {
      return startup.completed == cfg.result_ports.size();
    });
  }
  if (startup.failed) {
    cfg.logger_->critical(
        "At least one ZeroMQ result endpoint failed to start");
    zmq_ctx_shutdown(context);
    for (auto &receiver : receivers)
      receiver.join();
    zmq_ctx_term(context);
    return -1;
  }

  for (auto &receiver : receivers)
    receiver.join();

  return 0;
}
