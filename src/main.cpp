#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <pthread.h>
#include <Globalcfg.hpp>
#include <iostream>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
extern int dpdk();

// 线程入口函数
void *dpdk_thread(void *arg)
{
    int ret = dpdk();
    return NULL;
}
// extern int init();
int main()
{
    // init global config
    auto &cfg = GlobalConfig::getInstance();
    cfg.initlog();
    // read cfg from yaml
    if (!cfg.initFromYaml("config.yaml"))
    {
        return false;
    }

    // init memory
    // init();
    // start dpdk trhead
    pthread_t dpdk_t;
    pthread_create(&dpdk_t, NULL, dpdk_thread, NULL);
    pthread_join(dpdk_t, NULL);
    // while(1)
    // {

    // }
    // cfg.logger_->info("quit\n");
    
    return 0;
}