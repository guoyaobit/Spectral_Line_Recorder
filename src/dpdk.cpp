#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <rte_common.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_ring.h>

#include <Globalcfg.hpp>
#include <iostream>
#include <vector>
#include <fstream>
#include <iostream>

#include "VDIF.hpp"
#include "readerwriterqueue.h"
#include "readerwritercircularbuffer.h"
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <ctime>
#include "sdfits.h"
#include "ContinuumFits.h"
// #include "sdfits_writer.h"
#define nb_rxd_SIZE 8192
#define NUM_MBUFS 262144
#define MBUF_CACHE_SIZE 512
#define BURST_SIZE 128
#define RING_SIZE 8192
#define SPECTRUM_MAGIC 0x534C5231
#define SPECTRUM_VERSION_V1 1
#define SPECTRUM_VERSION_V2 2
constexpr uint16_t MAX_GLOBAL_SUBBAND_ID = 31;
constexpr uint16_t MAX_WINDOW_ID = 3;
constexpr uint32_t MAX_SPECTRUM_CHANNELS = 65536U * 256U;
constexpr size_t SPECTRUM_CHUNK_DATA_SIZE = 8192;

auto &cfg = GlobalConfig::getInstance();
constexpr size_t EXPECTED_PKT_LEN = 8266;
// const uint16_t port_list[] = {60000, 60001, 60002, 60003, 60004, 60005, 60006, 60007};
std::vector<rte_ring *> rx_rings;
rte_ring *continuum_ring = nullptr;
ContinuumFits m_continuum_fits;
struct lcore_param
{
    uint16_t port_id;
    uint16_t queue_id;
    uint16_t lcore_id;
    // uint16_t dest_port;
};

#define MAX_PORTS 2


static int
port_init(uint16_t port, struct rte_mempool *mbuf_pool, uint16_t nb_rx_queues)
{

    uint16_t nb_rxd = nb_rxd_SIZE;
    int retval;

    struct rte_eth_dev_info dev_info;
    rte_eth_dev_info_get(port, &dev_info);
    struct rte_eth_conf port_conf = {0};
    /* RX 多队列模式 */
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;

    port_conf.rxmode.mtu = 9000;
    uint16_t nb_tx_queues = 1; // 即使不发包也加一个
    retval = rte_eth_dev_configure(port, nb_rx_queues, nb_tx_queues, &port_conf);
    if (retval < 0)
        return retval;

    retval = rte_eth_tx_queue_setup(port, 0, nb_rxd_SIZE,
                                    rte_eth_dev_socket_id(port), NULL);
    if (retval < 0)
        return retval;

    for (uint16_t q = 0; q < nb_rx_queues; q++)
    {

        retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
                                        rte_eth_dev_socket_id(port), NULL, mbuf_pool);
        if (retval < 0)
            return retval;
    }

    retval = rte_eth_dev_start(port);
    if (retval < 0)
        return retval;
    
    // disable promisc
    rte_eth_promiscuous_disable(port);
    rte_eth_allmulticast_disable(port);
    return 0;
}
// packet recving  thread
static int
lcore_recv(void *arg)
{
    struct lcore_param *param = (struct lcore_param *)arg;

    unsigned lcore_id = rte_lcore_id();
    uint16_t port = param->port_id;
    uint16_t queue_id = param->queue_id;
    struct rte_mbuf *bufs[BURST_SIZE];
    uint16_t nb_rx;
    uint64_t t_num = 0;
    auto &cfg = GlobalConfig::getInstance();
    size_t pool_idx = 0;

    cfg.logger_->debug("Running locre_recv thread on port {} ,queue {} ,on core {}", port, queue_id, lcore_id);
    // auto fd = open("/data/dpdk_capture.bin", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    while (1)
    {
        nb_rx = rte_eth_rx_burst(port, queue_id, bufs, BURST_SIZE);
        if (nb_rx == 0)
            continue;

        for (int i = 0; i < nb_rx; i++)
        {
            struct rte_mbuf *mbuf = bufs[i];
            // size_t datalen = rte_pktmbuf_pkt_len(mbuf);
            /**prase udp header */
            struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
            if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
            {
                rte_pktmbuf_free(mbuf);
                continue;
            }
            struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
            if (ip_hdr->next_proto_id != IPPROTO_UDP)
            {
                rte_pktmbuf_free(mbuf);
                continue;
            }
            rte_ring *ring = rx_rings[queue_id];
            if (rte_ring_enqueue(ring, mbuf) != 0)
            {
                static std::atomic<uint64_t> ring_drop_count{0};
                const uint64_t count = ring_drop_count.fetch_add(1) + 1;
                if (count == 1 || count % 100000 == 0)
                {
                    cfg.logger_->warn(
                        "Dropped packets because RX ring {} is full; count={}",
                        queue_id, count);
                }
                rte_pktmbuf_free(mbuf);
            }
        }
    }
    return 0;
}

#define SPECTRUM_HEADER_SIZE (sizeof(spectrum_header))

struct SpectrumFrame
{
    uint16_t total_pkt, received_pkt;
    uint32_t n_channels;
    spectrum_header header;
    std::vector<std::vector<float>> packets;
    std::chrono::steady_clock::time_point created_at;
    explicit SpectrumFrame(const spectrum_header &first_header)
        : total_pkt(first_header.total_pkt), received_pkt(0),
          n_channels(first_header.n_channels), header(first_header),
          packets(first_header.total_pkt),
          created_at(std::chrono::steady_clock::now()) {}
};

struct FrameKey
{
    uint64_t timestamp_ns;
    uint32_t obs_id;
    uint32_t integration_id;
    uint16_t subband_id;
    uint16_t window_id;
    uint8_t beam_id;

    bool operator==(const FrameKey &o) const noexcept
    {
        return timestamp_ns == o.timestamp_ns &&
               obs_id == o.obs_id &&
               integration_id == o.integration_id &&
               subband_id == o.subband_id &&
               window_id == o.window_id &&
               beam_id == o.beam_id;
    }
};

struct FrameKeyHash
{
    size_t operator()(const FrameKey &k) const noexcept
    {
        size_t h = std::hash<uint64_t>{}(k.timestamp_ns);
        h ^= std::hash<uint32_t>{}(k.obs_id) + 0x9e3779b9 +
             (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(k.integration_id) + 0x9e3779b9 +
             (h << 6) + (h >> 2);
        h ^= std::hash<uint16_t>{}(k.subband_id) + 0x9e3779b9 +
             (h << 6) + (h >> 2);
        h ^= std::hash<uint16_t>{}(k.window_id) + 0x9e3779b9 +
             (h << 6) + (h >> 2);
        h ^= std::hash<uint8_t>{}(k.beam_id) + 0x9e3779b9 +
             (h << 6) + (h >> 2);
        return h;
    }
};

std::unordered_map<FrameKey, SpectrumFrame *, FrameKeyHash> frame_map;
struct BandKey
{
    uint16_t subband_id;
    uint16_t window_id;
    uint8_t beam_id;
    double f_start; // 起始频率（Hz）
    double f_stop;  // 截止频率（Hz）
    double channel_bw_hz;
    uint32_t n_channels;

    bool operator==(const BandKey &o) const noexcept
    {
        return subband_id == o.subband_id &&
               window_id == o.window_id &&
               beam_id == o.beam_id &&
               f_start == o.f_start &&
               f_stop == o.f_stop &&
               channel_bw_hz == o.channel_bw_hz &&
               n_channels == o.n_channels;
    }
};

struct BandKeyHash
{
    size_t operator()(const BandKey &k) const noexcept
    {
        size_t h = std::hash<uint16_t>{}(k.subband_id);
        auto combine = [&h](size_t value) {
            h ^= value + 0x9e3779b9 + (h << 6) + (h >> 2);
        };
        combine(std::hash<uint16_t>{}(k.window_id));
        combine(std::hash<uint8_t>{}(k.beam_id));
        combine(std::hash<double>{}(k.f_start));
        combine(std::hash<double>{}(k.f_stop));
        combine(std::hash<double>{}(k.channel_bw_hz));
        combine(std::hash<uint32_t>{}(k.n_channels));
        return h;
    }
};
void get_date_obs(uint64_t timestamp_ns, char date_obs[16])
{
    time_t sec = static_cast<time_t>(timestamp_ns / 1000000000ULL);

    struct tm utc_tm;
    gmtime_r(&sec, &utc_tm);

    snprintf(date_obs,
             16,
             "%02d/%02d/%02d",
             utc_tm.tm_mday,
             utc_tm.tm_mon + 1,
             utc_tm.tm_year % 100);
}
struct WriterState
{
    sdfits writer;
    std::mutex mutex;
};

std::unordered_map<BandKey, std::shared_ptr<WriterState>, BandKeyHash> writers;
std::mutex frame_map_mutex;
std::mutex writers_map_mutex;
std::mutex cfitsio_mutex;
std::chrono::steady_clock::time_point last_frame_cleanup =
    std::chrono::steady_clock::now();

bool cfitsio_supports_parallel_io()
{
    static const bool supported = fits_is_reentrant() != 0;
    return supported;
}

bool same_frame_metadata(const spectrum_header &a,
                         const spectrum_header &b)
{
    return a.magic == b.magic && a.version == b.version &&
           a.timestamp_ns == b.timestamp_ns && a.obs_id == b.obs_id &&
           a.integration_id == b.integration_id &&
           a.subband_id == b.subband_id && a.window_id == b.window_id &&
           a.subband_start_freq == b.subband_start_freq &&
           a.subband_end_freq == b.subband_end_freq &&
           a.start_freq_hz == b.start_freq_hz &&
           a.channel_bw_hz == b.channel_bw_hz &&
           a.n_channels == b.n_channels && a.stokes == b.stokes &&
           a.total_pkt == b.total_pkt && a.exposure == b.exposure &&
           a.noise_state == b.noise_state && a.cal_mode == b.cal_mode &&
           a.beam_id == b.beam_id && a.ra == b.ra && a.dec == b.dec &&
           a.flags == b.flags;
}

// 核心函数：接收 UDP 包 + 多包重组 + 合并 + 写文件
void receive_packet(const spectrum_header &pkthdr, const float *payload, size_t payload_len_bytes)
{
    if (pkthdr.n_channels == 0 ||
        pkthdr.n_channels > MAX_SPECTRUM_CHANNELS)
    {
        cfg.logger_->warn(
            "Dropped spectrum fragment with invalid channel count: "
            "subband={}, window={}, channels={}",
            pkthdr.subband_id, pkthdr.window_id, pkthdr.n_channels);
        return;
    }

    const size_t expected_frame_bytes =
        static_cast<size_t>(pkthdr.n_channels) * 4 * sizeof(float);
    const size_t expected_total_pkt =
        (expected_frame_bytes + SPECTRUM_CHUNK_DATA_SIZE - 1) /
        SPECTRUM_CHUNK_DATA_SIZE;
    const size_t fragment_offset =
        static_cast<size_t>(pkthdr.pkt_id) * SPECTRUM_CHUNK_DATA_SIZE;
    const size_t expected_fragment_bytes =
        fragment_offset < expected_frame_bytes
            ? std::min(SPECTRUM_CHUNK_DATA_SIZE,
                       expected_frame_bytes - fragment_offset)
            : 0;

    if (pkthdr.total_pkt == 0 || pkthdr.pkt_id >= pkthdr.total_pkt ||
        pkthdr.total_pkt != expected_total_pkt ||
        payload_len_bytes != expected_fragment_bytes ||
        payload_len_bytes % sizeof(float) != 0)
    {
        cfg.logger_->warn(
            "Dropped invalid spectrum fragment: subband={}, window={}, "
            "packet={}/{}, channels={}, payload_bytes={}",
            pkthdr.subband_id, pkthdr.window_id, pkthdr.pkt_id,
            pkthdr.total_pkt, pkthdr.n_channels, payload_len_bytes);
        return;
    }

    FrameKey key{pkthdr.timestamp_ns, pkthdr.obs_id,
                 pkthdr.integration_id, pkthdr.subband_id,
                 pkthdr.window_id, pkthdr.beam_id};
    std::vector<float> full;
    {
        std::lock_guard<std::mutex> lock(frame_map_mutex);
        const auto now = std::chrono::steady_clock::now();
        if (now - last_frame_cleanup >= std::chrono::seconds(5))
        {
            size_t expired = 0;
            for (auto it = frame_map.begin(); it != frame_map.end();)
            {
                if (now - it->second->created_at >= std::chrono::seconds(30))
                {
                    delete it->second;
                    it = frame_map.erase(it);
                    ++expired;
                }
                else
                {
                    ++it;
                }
            }
            if (expired != 0)
                cfg.logger_->warn(
                    "Discarded {} incomplete spectrum frame(s) after timeout",
                    expired);
            last_frame_cleanup = now;
        }

        SpectrumFrame *frame;
        auto it = frame_map.find(key);
        if (it == frame_map.end())
        {
            frame = new SpectrumFrame(pkthdr);
            frame_map[key] = frame;
        }
        else
        {
            frame = it->second;
            if (frame->total_pkt != pkthdr.total_pkt ||
                frame->n_channels != pkthdr.n_channels ||
                !same_frame_metadata(frame->header, pkthdr))
            {
                cfg.logger_->warn(
                    "Dropped inconsistent spectrum fragment for subband={}, "
                    "window={}",
                    pkthdr.subband_id, pkthdr.window_id);
                return;
            }
        }

        auto &packet = frame->packets[pkthdr.pkt_id];
        if (!packet.empty())
        {
            cfg.logger_->warn(
                "Ignored duplicate spectrum fragment: subband={}, window={}, "
                "packet={}",
                pkthdr.subband_id, pkthdr.window_id, pkthdr.pkt_id);
            return;
        }

        packet.resize(payload_len_bytes / sizeof(float));
        memcpy(packet.data(), payload, payload_len_bytes);
        frame->received_pkt++;
        if (frame->received_pkt != frame->total_pkt)
            return;

        full.reserve(static_cast<size_t>(frame->n_channels) * 4);
        for (auto &fragment : frame->packets)
            full.insert(full.end(), fragment.begin(), fragment.end());

        delete frame;
        frame_map.erase(key);
    }

    const size_t expected_values = static_cast<size_t>(pkthdr.n_channels) * 4;
    if (full.size() != expected_values)
    {
        cfg.logger_->warn(
            "Dropped completed spectrum frame with wrong payload size: "
            "subband={}, window={}, expected_values={}, actual_values={}",
            pkthdr.subband_id, pkthdr.window_id, expected_values, full.size());
        return;
    }

    const double f_start = pkthdr.start_freq_hz;
    const double f_stop =
        f_start + pkthdr.channel_bw_hz * pkthdr.n_channels;
    BandKey filekey{
        pkthdr.subband_id,
        pkthdr.window_id,
        pkthdr.beam_id,
        f_start,
        f_stop,
        pkthdr.channel_bw_hz,
        pkthdr.n_channels};

    {
        std::shared_ptr<WriterState> writer_state;
        {
            // Protect only the writer table and one-time file creation. Once
            // a writer is published, unrelated bands must not block each
            // other while CFITSIO writes and flushes their files.
            std::lock_guard<std::mutex> map_lock(writers_map_mutex);
            auto writer_it = writers.find(filekey);
            if (writer_it == writers.end())
            {
                writer_state = std::make_shared<WriterState>();
                sdfits &writer = writer_state->writer;
                writer.new_file = 1;

                const char beam_name = pkthdr.beam_id == 0 ? 'A' : 'B';
                const int filename_length = snprintf(
                    writer.basefilename, sizeof(writer.basefilename),
                    "%s/sb%02u_w%u_%.2f-%.2fMHz_%c_%u",
                    cfg.folder.c_str(),
                    static_cast<unsigned>(pkthdr.subband_id),
                    static_cast<unsigned>(pkthdr.window_id), f_start / 1e6,
                    f_stop / 1e6, beam_name,
                    static_cast<unsigned>(pkthdr.n_channels));
                if (filename_length < 0 ||
                    static_cast<size_t>(filename_length) >=
                        sizeof(writer.basefilename))
                {
                    cfg.logger_->error(
                        "SDFITS base filename is too long for subband {}, "
                        "window {}",
                        pkthdr.subband_id, pkthdr.window_id);
                    return;
                }

                writer.hdr.nchan = pkthdr.n_channels;
                get_date_obs(
                    pkthdr.timestamp_ns -
                        static_cast<uint64_t>(pkthdr.exposure * 0.5 * 1e9),
                    writer.hdr.date_obs);
                writer.hdr.chan_bw = pkthdr.channel_bw_hz;
                writer.hdr.obsfreq =
                    pkthdr.start_freq_hz +
                    pkthdr.channel_bw_hz * pkthdr.n_channels / 2;
                writer.hdr.nsubband = 1;
                writer.hdr.npol = 4;
                std::unique_lock<std::mutex> cfitsio_lock(
                    cfitsio_mutex, std::defer_lock);
                if (!cfitsio_supports_parallel_io())
                    cfitsio_lock.lock();
                const int create_status = writer.sdfits_create();
                if (create_status != 0)
                {
                    cfg.logger_->error(
                        "Failed to create SDFITS file for subband {}, window "
                        "{}, beam {}, status={}",
                        pkthdr.subband_id, pkthdr.window_id, beam_name,
                        create_status);
                    return;
                }
                writers.emplace(filekey, writer_state);
                cfg.logger_->info(
                    "Created SDFITS output for subband {}, window {}, beam "
                    "{}: {}",
                    pkthdr.subband_id, pkthdr.window_id, beam_name,
                    writer.filename);
            }
            else
            {
                writer_state = writer_it->second;
            }
        }

        std::lock_guard<std::mutex> writer_lock(writer_state->mutex);
        std::unique_lock<std::mutex> cfitsio_lock(
            cfitsio_mutex, std::defer_lock);
        if (!cfitsio_supports_parallel_io())
            cfitsio_lock.lock();
        sdfits &writer = writer_state->writer;
        writer.data_columns.data =
            reinterpret_cast<unsigned char *>(full.data());
        writer.data_columns.cal_on = pkthdr.noise_state;
        writer.data_columns.integ_num =
            static_cast<int>(pkthdr.integration_id);
        writer.data_columns.centre_freq[0] = writer.hdr.obsfreq;
        writer.data_columns.time =
            40587 + pkthdr.timestamp_ns / 1e9 / 86400;
        writer.data_columns.exposure = pkthdr.exposure;
        const int write_status = writer.sdfits_write_subint();
        if (write_status != 0)
        {
            cfg.logger_->error(
                "Failed to write SDFITS row to {}, status={}",
                writer.filename, write_status);
        }
        writer.data_columns.data = nullptr;
    }
}


// 获取指定网卡的端口号
inline int get_port_by_name(const std::string &name)
{
    uint16_t nb_ports = rte_eth_dev_count_avail();
    for (uint16_t port = 0; port < nb_ports; ++port)
    {
        char port_name[RTE_ETH_NAME_MAX_LEN];
        if (rte_eth_dev_get_name_by_port(port, port_name) == 0)
        {
            if (name == port_name)
                return port;
        }
    }
    return -1;
}

std::vector<lcore_param> generate_lcore_params(const std::vector<uint16_t> &port_ids,
                                               uint16_t queues_per_port,
                                               //    uint16_t start_dest_port,
                                               bool include_lcore0 = false)
{
    std::vector<lcore_param> params;
    std::vector<unsigned> next_lcore(RTE_MAX_NUMA_NODES, include_lcore0 ? 0 : 1);

    std::vector<unsigned> enabled_lcores;
    unsigned lcore_id;
    RTE_LCORE_FOREACH(lcore_id)
    {
        enabled_lcores.push_back(lcore_id);
    }

    if (enabled_lcores.empty())
    {
        throw std::runtime_error("No enabled lcores found!");
    }

    for (auto port : port_ids)
    {
        uint16_t nb_ports = rte_eth_dev_count_avail();
        if (port >= nb_ports)
        {
            throw std::runtime_error("Invalid port_id: " + std::to_string(port));
        }

        for (uint16_t q = 0; q < queues_per_port; ++q)
        {
            unsigned assigned_lcore = RTE_MAX_LCORE;
            uint16_t port_socket = rte_eth_dev_socket_id(port);
            
            // 优先在同 NUMA 节点分配
            for (unsigned lc = next_lcore[port_socket]; lc < RTE_MAX_LCORE; ++lc)
            {
                if (rte_lcore_is_enabled(lc) &&
                    rte_lcore_to_socket_id(lc) == port_socket)
                {
                    assigned_lcore = lc;
                    next_lcore[port_socket] = lc + 1;
                    break;
                }
            }

            // 如果该 NUMA 节点不足，从其他 NUMA 节点分配
            if (assigned_lcore == RTE_MAX_LCORE)
            {
                for (auto lc : enabled_lcores)
                {
                    if (lc >= next_lcore[port_socket])
                        continue; // 避免重复
                    assigned_lcore = lc;
                    break;
                }
            }

            if (assigned_lcore == RTE_MAX_LCORE)
            {
                throw std::runtime_error("Not enough lcores for port " +
                                         std::to_string(port) +
                                         ", queue " + std::to_string(q));
            }

            lcore_param p;
            p.port_id = port;
            p.queue_id = q;
            p.lcore_id = assigned_lcore;
            // p.dest_port = start_dest_port + q;
            params.push_back(p);
        }
    }

    return params;
}

struct ContinuumResult
{
    uint64_t timestamp_ns;
    uint16_t subband_id;
    float power;
    float exposure;
    uint8_t noise_state;
};
struct ContinuumFrame
{
    // 这个值作为本积分周期的代表时间
    uint64_t timestamp_ns = 0;

    float power = 0.0;
    uint32_t received_subbands = 0;

    // 防止同一个子带重复计入
    std::unordered_set<uint16_t> subbands;

    float exposure = 0.0;
    uint8_t noise_state = 0;
};

std::map<uint64_t, ContinuumFrame> continuum_map;
constexpr uint64_t CONTINUUM_TIME_TOLERANCE_NS = 1000;
std::map<uint64_t, ContinuumFrame>::iterator
find_continuum_frame(uint64_t timestamp_ns)
{
    auto it = continuum_map.lower_bound(timestamp_ns);

    if (it != continuum_map.end())
    {
        uint64_t diff =
            (it->first >= timestamp_ns)
                ? (it->first - timestamp_ns)
                : (timestamp_ns - it->first);

        if (diff <= CONTINUUM_TIME_TOLERANCE_NS)
            return it;
    }

    if (it != continuum_map.begin())
    {
        auto prev = std::prev(it);

        uint64_t diff =
            (prev->first >= timestamp_ns)
                ? (prev->first - timestamp_ns)
                : (timestamp_ns - prev->first);

        if (diff <= CONTINUUM_TIME_TOLERANCE_NS)
            return prev;
    }

    return continuum_map.end();
}
static int
recv2mem(void *args)
{
    
    struct lcore_param *param = (struct lcore_param *)args;
    int stream_id = param->queue_id;
    rte_ring *ring = rx_rings[stream_id];
    rte_mbuf *mbuf;

    while (1)
    {
        int ret = rte_ring_dequeue(ring, (void **)&mbuf);
        if (ret != 0)
        {
            continue;
        }
        const uint32_t pkt_len = rte_pktmbuf_pkt_len(mbuf);
        const uint32_t data_len = rte_pktmbuf_data_len(mbuf);
        if (mbuf->nb_segs != 1 ||
            data_len < sizeof(rte_ether_hdr) + sizeof(rte_ipv4_hdr) +
                           sizeof(rte_udp_hdr) + sizeof(spectrum_header))
        {
            rte_pktmbuf_free(mbuf);
            continue;
        }

        uint8_t *packet_data = rte_pktmbuf_mtod(mbuf, uint8_t *);
        const auto *eth_hdr =
            reinterpret_cast<const rte_ether_hdr *>(packet_data);
        const auto *ip_hdr = reinterpret_cast<const rte_ipv4_hdr *>(
            packet_data + sizeof(rte_ether_hdr));
        const size_t ip_header_len =
            static_cast<size_t>(ip_hdr->version_ihl & 0x0f) * 4;
        const size_t udp_offset = sizeof(rte_ether_hdr) + ip_header_len;
        const size_t payload_offset = udp_offset + sizeof(rte_udp_hdr);
        if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4) ||
            ip_hdr->next_proto_id != IPPROTO_UDP ||
            ip_header_len < sizeof(rte_ipv4_hdr) ||
            payload_offset + sizeof(spectrum_header) > data_len)
        {
            rte_pktmbuf_free(mbuf);
            continue;
        }

        const auto *udp_hdr = reinterpret_cast<const rte_udp_hdr *>(
            packet_data + udp_offset);
        const size_t udp_len = rte_be_to_cpu_16(udp_hdr->dgram_len);
        if (udp_len < sizeof(rte_udp_hdr) + sizeof(spectrum_header) ||
            udp_offset + udp_len > pkt_len ||
            udp_offset + udp_len > data_len)
        {
            rte_pktmbuf_free(mbuf);
            continue;
        }
        uint8_t *udp_payload = packet_data + payload_offset;

        spectrum_header pkthdr;
        memcpy(&pkthdr, udp_payload, sizeof(spectrum_header));
        const bool supported_version =
            pkthdr.version == SPECTRUM_VERSION_V1 ||
            pkthdr.version == SPECTRUM_VERSION_V2;
        if (pkthdr.magic != SPECTRUM_MAGIC || !supported_version)
        {
            static std::atomic<uint64_t> invalid_header_count{0};
            const uint64_t count = invalid_header_count.fetch_add(1) + 1;
            if (count == 1 || count % 100000 == 0)
            {
                cfg.logger_->warn(
                    "Dropped spectrum packets with invalid header: "
                    "magic=0x{:08x}, version={}, count={}",
                    pkthdr.magic, pkthdr.version, count);
            }
            rte_pktmbuf_free(mbuf);
            continue;
        }
        if (pkthdr.version == SPECTRUM_VERSION_V1)
        {
            static std::atomic_flag warned_legacy_v1 = ATOMIC_FLAG_INIT;
            if (!warned_legacy_v1.test_and_set())
            {
                cfg.logger_->warn(
                    "Receiving legacy spectrum protocol v1; beam metadata is "
                    "unavailable, defaulting output to beam A");
            }
            pkthdr.beam_id = 0;
        }
        else if (pkthdr.beam_id > 1)
        {
            static std::atomic<uint64_t> invalid_beam_count{0};
            const uint64_t count = invalid_beam_count.fetch_add(1) + 1;
            if (count == 1 || count % 100000 == 0)
            {
                cfg.logger_->warn(
                    "Dropped spectrum protocol v2 packets with invalid "
                    "beam_id={}, count={}",
                    pkthdr.beam_id, count);
            }
            rte_pktmbuf_free(mbuf);
            continue;
        }
        if (pkthdr.subband_id > MAX_GLOBAL_SUBBAND_ID ||
            pkthdr.window_id > MAX_WINDOW_ID ||
            !std::isfinite(pkthdr.start_freq_hz) ||
            !std::isfinite(pkthdr.channel_bw_hz) ||
            pkthdr.channel_bw_hz <= 0.0 ||
            !std::isfinite(pkthdr.exposure) || pkthdr.exposure <= 0.0f)
        {
            static std::atomic<uint64_t> invalid_index_count{0};
            const uint64_t count = invalid_index_count.fetch_add(1) + 1;
            if (count == 1 || count % 100000 == 0)
            {
                cfg.logger_->warn(
                    "Dropped spectrum packets with invalid identity or "
                    "metadata: subband={}, window={}, beam={}, count={}",
                    pkthdr.subband_id, pkthdr.window_id, pkthdr.beam_id,
                    count);
            }
            rte_pktmbuf_free(mbuf);
            continue;
        }
        // payload 数据指针
        float *payload = reinterpret_cast<float *>(udp_payload + sizeof(spectrum_header));
        size_t payload_len_bytes =
            udp_len - sizeof(rte_udp_hdr) - sizeof(spectrum_header);
        if (cfg.observation_mode == ObservationMode::CONTINUUM)
        {
            if (payload_len_bytes < sizeof(float))
            {
                rte_pktmbuf_free(mbuf);
                continue;
            }
            float subband_power;
            memcpy(
                &subband_power,
                udp_payload + sizeof(spectrum_header),
                sizeof(float));
            auto *result = new ContinuumResult{
                pkthdr.timestamp_ns,
                pkthdr.subband_id,
                subband_power,
                pkthdr.exposure,
                pkthdr.noise_state
            };
            if (rte_ring_enqueue(continuum_ring, result) != 0)
                delete result;
            rte_pktmbuf_free(mbuf);
            continue;
        }
        else{
            receive_packet(pkthdr, payload, payload_len_bytes);
            rte_pktmbuf_free(mbuf);
        }
    }
}
void accumulate_continuum(const ContinuumResult &r)
{
    auto it = find_continuum_frame(r.timestamp_ns);

    // 没有找到时间上匹配的积分周期
    if (it == continuum_map.end())
    {
        ContinuumFrame frame;
        frame.timestamp_ns = r.timestamp_ns;
        frame.power = r.power;
        frame.received_subbands = 1;
        frame.subbands.insert(r.subband_id);
        frame.exposure = r.exposure;
        frame.noise_state = r.noise_state;

        continuum_map.emplace(r.timestamp_ns, std::move(frame));

        return;
    }

    ContinuumFrame &frame = it->second;

    // 防止同一个 subband 重复进入
    if (!frame.subbands.insert(r.subband_id).second)
    {
        cfg.logger_->warn(
            "Duplicate continuum result: "
            "frame_ts={}, result_ts={}, diff={} ns, subband={}",
            frame.timestamp_ns,
            r.timestamp_ns,
            static_cast<int64_t>(r.timestamp_ns) -
                static_cast<int64_t>(frame.timestamp_ns),
            r.subband_id);

        return;
    }

    // 时间匹配，但 timestamp 可以不同
    frame.power += r.power;
    ++frame.received_subbands;

    // 所有子带都已经收到
    if (frame.received_subbands == cfg.recv_streams)
    {
        const float total_power = frame.power;
        if(cfg.Debug_mode)
        {
            std::cout << "Continuum frame complete: "
                      << "timestamp_ns=" << frame.timestamp_ns
                      << ", total_power=" << total_power
                      << ", exposure=" << frame.exposure
                      << ", noise_state=" << static_cast<int>(frame.noise_state)
                      << std::endl;
        }

        m_continuum_fits.write(
            frame.timestamp_ns,
            total_power,
            frame.exposure,
            frame.noise_state);
        
        continuum_map.erase(it);
    }
}
static int continuum_worker(void *)
{
    ContinuumResult *result = nullptr;
    cfg.logger_->info("Continuum worker started on lcore {}", rte_lcore_id());
    const std::string filename =
        cfg.folder + "/continuum.fits";

    // worker 启动时只创建一次
    if (!m_continuum_fits.create(filename, 0))
    {
        cfg.logger_->error(
            "Failed to create continuum FITS: {}",
            filename);

        return -1;
    }
    while (1)
    {
        if (rte_ring_dequeue(
                continuum_ring,
                reinterpret_cast<void **>(&result)) != 0)
        {
            continue;
        }
        accumulate_continuum(*result);

        delete result;
    }

    return 0;
}
int dpdk()
{
    char *argv[] = {
        "7mm_recorder", // name
        "-n", "4",  // Mem_channels
        // "--",
        // "-p","0x3",
        NULL};
    int argc = sizeof(argv) / sizeof(argv[0]) - 1;

    unsigned lcore_id;
    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL init\n");

    struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL",
                                                            NUM_MBUFS * 2, MBUF_CACHE_SIZE, 0, 10240,
                                                            rte_socket_id());
    if (mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
    // init rings one subband to one ring
    auto &cfg = GlobalConfig::getInstance();
    rx_rings.resize(cfg.recv_streams, nullptr);
    for (int i = 0; i < cfg.recv_streams; i++)
    {
        char ring_name[32];
        snprintf(ring_name, sizeof(ring_name), "rx_ring_%d", i);
        rx_rings[i] = rte_ring_create(ring_name, RING_SIZE, SOCKET_ID_ANY, 0);
        if (rx_rings[i] == NULL)
        {
            rte_exit(EXIT_FAILURE, "Failed to create ring %s: %s\n", ring_name, rte_strerror(rte_errno));
        }
    }
    continuum_ring = rte_ring_create(
    "continuum_ring",
    RING_SIZE,
    SOCKET_ID_ANY,
    0);

    if (!continuum_ring)
    {
        rte_exit(
            EXIT_FAILURE,
            "Failed to create continuum ring: %s\n",
            rte_strerror(rte_errno));
    }
    // uint16_t queues_per_port = 8;
    // uint16_t start_dest_port = 60000;

    std::vector<uint16_t> ports = {0};
    auto lcore_params = generate_lcore_params(ports, cfg.recv_streams);
    std::unordered_set<unsigned> assigned_lcores;
    assigned_lcores.insert(rte_get_main_lcore());
    for (const auto &param : lcore_params)
    {
        if (!assigned_lcores.insert(param.lcore_id).second)
            rte_exit(EXIT_FAILURE,
                     "Duplicate or main lcore %u assigned to RX worker\n",
                     param.lcore_id);
    }

    std::vector<unsigned> processing_lcores;
    RTE_LCORE_FOREACH(lcore_id)
    {
        if (assigned_lcores.count(lcore_id) == 0)
            processing_lcores.push_back(lcore_id);
    }

    const size_t required_processing_lcores =
        lcore_params.size() +
        (cfg.observation_mode == ObservationMode::CONTINUUM ? 1 : 0);
    if (processing_lcores.size() < required_processing_lcores)
        rte_exit(EXIT_FAILURE,
                 "Not enough DPDK lcores for packet processing: need %zu, "
                 "found %zu\n",
                 required_processing_lcores, processing_lcores.size());

    std::vector<lcore_param> processing_params(lcore_params.size());
    for (size_t i = 0; i < processing_params.size(); ++i)
    {
        processing_params[i].queue_id = static_cast<uint16_t>(i);
        processing_params[i].lcore_id = processing_lcores[i];
    }
    const unsigned continuum_lcore =
        cfg.observation_mode == ObservationMode::CONTINUUM
            ? processing_lcores[lcore_params.size()]
            : RTE_MAX_LCORE;

    // Initialize the Ethernet device only after all worker assignments have
    // been validated, so a bad CPU layout cannot leave a started port behind.
    if (port_init(0, mbuf_pool, cfg.recv_streams) != 0)
        rte_exit(EXIT_FAILURE, " Cannot init port %" PRIu16 "\n", 0);

    for (size_t i = 0; i < lcore_params.size(); ++i)
    {
        const int launch_status = rte_eal_remote_launch(
            lcore_recv, &lcore_params[i], lcore_params[i].lcore_id);
        if (launch_status != 0)
            rte_exit(EXIT_FAILURE,
                     "Failed to launch RX worker for queue %zu on lcore %u: "
                     "%d\n",
                     i, lcore_params[i].lcore_id, launch_status);
        cfg.logger_->info("RX queue {} worker launched on lcore {}", i,
                          lcore_params[i].lcore_id);
    }
    for (size_t i = 0; i < processing_params.size(); ++i)
    {
        const int launch_status = rte_eal_remote_launch(
            recv2mem, &processing_params[i], processing_params[i].lcore_id);
        if (launch_status != 0)
            rte_exit(EXIT_FAILURE,
                     "Failed to launch packet processor for queue %zu on "
                     "lcore %u: %d\n",
                     i, processing_params[i].lcore_id, launch_status);
        cfg.logger_->info("RX queue {} processor launched on lcore {}", i,
                          processing_params[i].lcore_id);
    }

    if (cfitsio_supports_parallel_io())
        cfg.logger_->info(
            "CFITSIO is reentrant; separate output files write concurrently");
    else
        cfg.logger_->warn(
            "CFITSIO is not reentrant; output writes remain serialized for "
            "data safety");

    if (cfg.observation_mode == ObservationMode::CONTINUUM)
    {
        int ret = rte_eal_remote_launch(
            continuum_worker,
            nullptr,
            continuum_lcore);

        cfg.logger_->info(
            "continuum_worker remote_launch ret={}, lcore={}",
            ret,
            continuum_lcore);

        if (ret != 0)
        {
            cfg.logger_->error(
                "Failed to launch continuum_worker on lcore {}, ret={}",
                continuum_lcore,
                ret);
        }
    }
    rte_eal_mp_wait_lcore();
    return 0;
}
