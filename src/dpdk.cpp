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

#include "sdfits.h"
#include "sdfits_writer.h"
#define RX_RING_SIZE 8192
#define NUM_MBUFS 262144
#define MBUF_CACHE_SIZE 512
#define BURST_SIZE 128
#define RING_SIZE 8192
#define SPECTRUM_MAGIC 0x534C5231
#define SPECTRUM_VERSION 1
// #define recv_streams 4
auto &cfg = GlobalConfig::getInstance();
constexpr size_t EXPECTED_PKT_LEN = 8266;
// const uint16_t port_list[] = {60000, 60001, 60002, 60003, 60004, 60005, 60006, 60007};
std::vector<rte_ring *> rx_rings;
struct lcore_param
{
    uint16_t port_id;
    uint16_t queue_id;
    uint16_t lcore_id;
    // uint16_t dest_port;
};

#define MAX_PORTS 2

static inline uint32_t simple_port_hash(uint16_t dst_port)
{
    return (uint32_t)dst_port;
}

static struct rte_flow *
create_udp_dst_flow(uint16_t port_id, uint16_t dst_port, uint16_t queue_id)
{
    struct rte_flow_attr attr;
    struct rte_flow_item pattern[4];
    struct rte_flow_action action[2];
    struct rte_flow_item_udp udp_spec, udp_mask;
    struct rte_flow_action_queue queue = {.index = queue_id};
    struct rte_flow_error error;
    struct rte_flow *flow = NULL;

    memset(&attr, 0, sizeof(attr));
    attr.ingress = 1;  // 入方向流量
    attr.priority = 0; // 优先级（0最高）

    // pattern: ETH → IPv4 → UDP (目的端口匹配)
    memset(pattern, 0, sizeof(pattern));
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;

    memset(&udp_spec, 0, sizeof(udp_spec));
    memset(&udp_mask, 0, sizeof(udp_mask));
    udp_spec.hdr.dst_port = rte_cpu_to_be_16(dst_port);
    udp_mask.hdr.dst_port = 0xFFFF; // 完全匹配端口

    pattern[2].type = RTE_FLOW_ITEM_TYPE_UDP;
    pattern[2].spec = &udp_spec;
    pattern[2].mask = &udp_mask;

    pattern[3].type = RTE_FLOW_ITEM_TYPE_END;

    // action: 重定向到指定队列
    memset(action, 0, sizeof(action));
    action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
    action[0].conf = &queue;
    action[1].type = RTE_FLOW_ACTION_TYPE_END;

    flow = rte_flow_create(port_id, &attr, pattern, action, &error);
    if (!flow)
    {
        cfg.logger_->error("❌ Failed to create flow for UDP dport{} -> queue {}: {}\n",
                           dst_port, queue_id, error.message ? error.message : "(no msg)");
    }
    else
    {
        cfg.logger_->debug("✅ Flow created: UDP dport {} -> queue {}\n", dst_port, queue_id);
    }

    return flow;
}

static struct rte_flow *
create_catch_all_drop(uint16_t port_id)
{
    struct rte_flow_attr attr;
    struct rte_flow_item pattern[2];
    struct rte_flow_action action[2];
    struct rte_flow_error error;
    struct rte_flow *flow = NULL;

    memset(&attr, 0, sizeof(attr));
    attr.ingress = 1;
    attr.priority = 2; // 低优先级，最后匹配

    // 匹配所有
    memset(pattern, 0, sizeof(pattern));
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

    // 动作 = 丢弃
    memset(action, 0, sizeof(action));
    action[0].type = RTE_FLOW_ACTION_TYPE_DROP;
    action[1].type = RTE_FLOW_ACTION_TYPE_END;

    flow = rte_flow_create(port_id, &attr, pattern, action, &error);
    if (!flow)
    {
        cfg.logger_->debug("❌ Failed to create catch-all DROP flow: {}\n",
                           error.message ? error.message : "(no msg)");
    }
    else
    {
        cfg.logger_->debug("✅ Catch-all DROP flow created (all other packets dropped)\n");
    }

    return flow;
}

static int
port_init(uint16_t port, struct rte_mempool *mbuf_pool, uint16_t nb_rx_queues)
{

    uint16_t nb_rxd = RX_RING_SIZE;
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

    retval = rte_eth_tx_queue_setup(port, 0, RX_RING_SIZE,
                                    rte_eth_dev_socket_id(port), NULL);
    if (retval < 0)
        return retval;

    // retval = rte_eth_dev_configure(port, nb_rx_queues, 0, &port_conf);
    // if (retval < 0)
    //     return retval;

    struct rte_eth_rxconf rxconf;
    rxconf = dev_info.default_rxconf;
    rxconf.rx_free_thresh = 1024;
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
    // for (int i = 0; i < nb_rx_queues; i++)
    // {
    //     create_udp_dst_flow(port, 60000 + i, i);
    // }
    // catch-all
    // create_catch_all_flow(port,i+1);
    // create_catch_all_drop(port);
    // printf("🚀 Port %d ready, listening on UDP 60000-60003\n", port);

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
    // int static_dstport = port_list[queue_id];
    // uint64_t global_packet_id = 0;
    // int presecond = -1;
    // int batch_idx = 0;
    // int pkt_idx_inbatch = 0;
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
            struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)((unsigned char *)ip_hdr + sizeof(struct rte_ipv4_hdr));
            // uint32_t dst_ip = rte_be_to_cpu_32(ip_hdr->dst_addr);
            uint16_t dst_port = rte_be_to_cpu_16(udp_hdr->dst_port);
            rte_ring *ring = rx_rings[queue_id];
            rte_ring_enqueue(ring, mbuf);
        }
    }
    return 0;
}

#define SPECTRUM_HEADER_SIZE (sizeof(spectrum_header))

typedef struct
{
    uint16_t total_pkt;     // 总包数
    uint16_t received_pkt;  // 已收到的包数
    uint8_t **packet_array; // 存放每个包指针
    uint32_t *packet_len;   // 每个包长度
    uint64_t timestamp_ns;
    uint16_t subband_id; // 子频段编号
    uint16_t window_id;  // window id
    time_t last_update;  // 超时回收
} spectrum_frame_t;

struct SpectrumFrame
{
    uint16_t total_pkt, received_pkt;
    uint32_t n_channels;
    std::vector<std::vector<float>> packets;
    SpectrumFrame(uint16_t total, uint32_t nchan)
        : total_pkt(total), received_pkt(0), n_channels(nchan), packets(total) {}
};

struct FrameKey
{
    uint64_t ts;                 // 时间戳
    uint32_t subband_start_freq; // 子带起始频率
    uint32_t subband_end_freq;   // 子带结束频率
    uint16_t sub;                // 子带 ID
    uint16_t win;                // 窗口 ID

    // Equality operator
    bool operator==(const FrameKey &o) const noexcept
    {
        return ts == o.ts &&
               subband_start_freq == o.subband_start_freq &&
               subband_end_freq == o.subband_end_freq &&
               sub == o.sub &&
               win == o.win;
    }
};

struct FrameKeyHash
{
    size_t operator()(const FrameKey &k) const noexcept
    {
        size_t h1 = std::hash<uint64_t>()(k.ts);
        size_t h2 = std::hash<uint32_t>()(k.subband_start_freq);
        size_t h3 = std::hash<uint32_t>()(k.subband_end_freq);
        size_t h4 = std::hash<uint16_t>()(k.sub);
        size_t h5 = std::hash<uint16_t>()(k.win);

        // 简单位移+异或组合
        return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3) ^ (h5 << 4);
    }
};

std::unordered_map<FrameKey, SpectrumFrame *, FrameKeyHash> frame_map;
struct BandKey
{
    double f_start; // 起始频率（Hz）
    double f_stop;  // 截止频率（Hz）

    bool operator==(const BandKey &o) const noexcept
    {
        return f_start == o.f_start && f_stop == o.f_stop;
    }
};
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
struct BandKeyHash
{
    size_t operator()(const BandKey &k) const noexcept
    {
        auto h1 = std::hash<long long>()(static_cast<long long>(k.f_start));
        auto h2 = std::hash<long long>()(static_cast<long long>(k.f_stop));
        return h1 ^ (h2 << 1);
    }
};
std::unordered_map<BandKey, sdfits *, BandKeyHash> writers;
// 核心函数：接收 UDP 包 + 多包重组 + 合并 + 写文件
void receive_packet(const spectrum_header &pkthdr, const float *payload, size_t payload_len_bytes)
{
    FrameKey key{pkthdr.timestamp_ns, pkthdr.subband_start_freq, pkthdr.subband_end_freq, pkthdr.window_id};
    SpectrumFrame *frame;

    auto it = frame_map.find(key);
    if (it == frame_map.end())
    {
        // not found
        frame = new SpectrumFrame(pkthdr.total_pkt, pkthdr.n_channels);
        frame_map[key] = frame;
    }
    else
        frame = it->second;

    // 保存当前包数据
    frame->packets[pkthdr.pkt_id].assign(payload, payload + payload_len_bytes / sizeof(float));
    frame->received_pkt++;

    // 完整帧处理
    if (frame->received_pkt == frame->total_pkt)
    {

        std::vector<float> full;
        full.reserve(frame->n_channels);
        for (auto &pkt : frame->packets)
            full.insert(full.end(), pkt.begin(), pkt.end());

        double f_start = pkthdr.start_freq_hz;
        double f_stop = f_start + pkthdr.channel_bw_hz * pkthdr.n_channels;

        BandKey filekey{pkthdr.start_freq_hz, pkthdr.channel_bw_hz};
        // SDFITSWriter *writer = nullptr;
        sdfits *writer = nullptr;
        // // 查找是否已经存在文件
        auto it = writers.find(filekey);
        if (it == writers.end())
        {
            // 生成文件名：f_start_f_stopMHz.fits
            // char fname[128];
            // sprintf(fname, "%.2f_%.2fMHz_%d.fits", f_start / 1e6, f_stop / 1e6,pkthdr.n_channels);

            // 创建新的写入器
            // writer = new SDFITSWriter(fname,pkthdr);
            writer = new sdfits();
            writer->new_file = 1;
            // writer->basefilename = fname;
            // strncpy(writer->basefilename,cfg.folder.c_str(),cfg.folder.length());
            std::string source_on="OFF";
            if(cfg.source_on)
                source_on = "ON";
            sprintf(writer->basefilename, "%s/%s_%s_%s/%.2f_%.2fMHz_%d.fits",
                cfg.folder.c_str(),
                cfg.object.c_str(),source_on.c_str(),
                getTimeString().c_str(),
                f_start / 1e6, f_stop / 1e6, pkthdr.n_channels);
            writers[filekey] = writer;
            writer->hdr.nchan = pkthdr.n_channels;
            writer->hdr.chan_bw = pkthdr.channel_bw_hz;
            writer->hdr.obsfreq = pkthdr.start_freq_hz+pkthdr.channel_bw_hz*pkthdr.n_channels/2;
            writer->hdr.nsubband = 1;
            writer->hdr.npol = 4;
            writer->sdfits_create();
            // cfg.logger_->info("create filename {}",writer->filename);
        }
        else
        {
            writer = it->second;
        }
        writer->data_columns.data = (unsigned char *)full.data();
        writer->data_columns.cal_on = pkthdr.noise_state;
        writer->data_columns.time = 40587 + pkthdr.timestamp_ns/1e9/86400; // 转换为秒
        writer->data_columns.exposure = pkthdr.exposure;
        // printf("%d\n",pkthdr.noise_state);
        writer->sdfits_write_subint();
        delete frame;
        frame_map.erase(key);
    }
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
        uint8_t *udp_payload = rte_pktmbuf_mtod_offset(mbuf, uint8_t *, 42);

        spectrum_header pkthdr;
        memcpy(&pkthdr, udp_payload, sizeof(spectrum_header));
        if(pkthdr.magic != SPECTRUM_MAGIC || pkthdr.version != SPECTRUM_VERSION)
        {
            rte_pktmbuf_free(mbuf);
            continue;
        }
        // payload 数据指针
        float *payload = reinterpret_cast<float *>(udp_payload + sizeof(spectrum_header));
        int pkt_len = rte_pktmbuf_pkt_len(mbuf);
        if(pkt_len <= 42 + sizeof(spectrum_header))
        {
            rte_pktmbuf_free(mbuf);
            continue;
        }
        size_t payload_len_bytes =
            pkt_len - 42 - sizeof(spectrum_header);
        if (pkthdr.pkt_id >= pkthdr.total_pkt)
        {
            rte_pktmbuf_free(mbuf);
            continue;
        }
        if(payload_len_bytes % sizeof(float) != 0)
        {
            rte_pktmbuf_free(mbuf);
            continue;
        }
        receive_packet(pkthdr, payload, payload_len_bytes);

        rte_pktmbuf_free(mbuf);
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
    // rx_rings.resize(cfg.recv_streams, nullptr);
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
    // uint16_t queues_per_port = 8;
    // uint16_t start_dest_port = 60000;

    std::vector<uint16_t> ports = {0};
    // 初始化端口 0
    if (port_init(0, mbuf_pool, cfg.recv_streams) != 0)
        rte_exit(EXIT_FAILURE, " Cannot init port %" PRIu16 "\n", 0);

    // init port config
    auto lcore_params = generate_lcore_params(ports, cfg.recv_streams);
    int lastcore_id;
    for (int i = 0; i < lcore_params.size(); ++i)
    {
        rte_eal_remote_launch(lcore_recv, &lcore_params[i], lcore_params[i].lcore_id);
        lastcore_id = lcore_params[i].lcore_id + 1;
    }
    for (int i = 0; i < lcore_params.size(); ++i)
    {
        struct lcore_param *recv_param = new lcore_param;
        recv_param->lcore_id = lastcore_id + i;
        recv_param->queue_id = i;

        rte_eal_remote_launch(recv2mem, (void *)recv_param, recv_param->lcore_id);
    }

    rte_eal_mp_wait_lcore();
    return 0;
}
