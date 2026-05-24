#include "http_server.h"
#include "job_scheduler.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {

int getenv_port() {
    const char* value = std::getenv("DCC_PORT");
    if (!value || !*value) {
        return 8080;
    }
    try {
        return std::max(1, std::stoi(value));
    } catch (...) {
        return 8080;
    }
}

} // namespace

int main() {
    try {
        dcc::RuntimeConfig config = dcc::runtime_config_from_env();
        std::cerr << "teamCode=" << config.team_code
                  << " dataPath=" << config.data_path
                  << " outputDir=" << config.output_dir
                  << " jobWorkers=" << config.job_workers
                  << " computeThreads=" << config.compute_threads
                  << " queueCoalesceMs=" << config.queue_coalesce_ms
                  << " tileRows=" << config.tile_rows
                  << " callback=" << (config.callback_enabled ? "enabled" : "disabled")
                  << "\n";

        dcc::JobScheduler scheduler(config);
        scheduler.start();

        dcc::HttpServer server(getenv_port(), scheduler);
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
