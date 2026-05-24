#pragma once

#include "job_scheduler.h"

#include <atomic>

namespace dcc {

class HttpServer {
public:
    HttpServer(int port, JobScheduler& scheduler);
    void run();
    void stop();

private:
    int port_;
    JobScheduler& scheduler_;
    std::atomic<bool> stopping_{false};
    int listen_fd_ = -1;

    void handle_client(int fd);
};

} // namespace dcc
