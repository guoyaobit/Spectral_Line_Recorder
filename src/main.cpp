#include <Globalcfg.hpp>

extern int receive_results();
extern void initialize_recorder_cpu_affinity();

int main()
{
    initialize_recorder_cpu_affinity();
    auto &cfg = GlobalConfig::getInstance();
    cfg.initlog();
    if (!cfg.initFromYaml("config.yaml"))
        return 1;

    return receive_results() == 0 ? 0 : 1;
}
