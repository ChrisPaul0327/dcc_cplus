#pragma once

#include "data_store.h"
#include "request.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace dcc {

struct RuntimeConfig {
    std::string team_code = "baseline";
    std::string data_path = "/dcc/root/table_data.csv";
    std::string output_dir;
    std::string callback_url = "http://dcc08-data-encrypt.paas.cmbchina.cn/callback";
    int job_workers = 4;
    int compute_threads = 1;
    int queue_coalesce_ms = 0;
    std::size_t tile_rows = 100000;
    bool callback_enabled = true;
};

struct QueuedJob {
    std::uint64_t priority = 0;
    std::uint64_t sequence = 0;
    EncryptRequest request;
};

struct QueuedJobLess {
    bool operator()(const QueuedJob& a, const QueuedJob& b) const {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        return a.sequence > b.sequence;
    }
};

class JobScheduler {
public:
    explicit JobScheduler(RuntimeConfig config);
    ~JobScheduler();

    JobScheduler(const JobScheduler&) = delete;
    JobScheduler& operator=(const JobScheduler&) = delete;

    void start();
    void stop();
    void enqueue(EncryptRequest request);
    void warmup_async();

private:
    RuntimeConfig config_;
    DataStore store_;
    std::once_flag load_once_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::priority_queue<QueuedJob, std::vector<QueuedJob>, QueuedJobLess> queue_;
    std::uint64_t next_sequence_ = 0;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
    std::once_flag warmup_once_;
    std::thread warmup_thread_;

    void worker_loop(int worker_id);
    void process_with_retry(const EncryptRequest& request);
    void process_once(const EncryptRequest& request);
    void ensure_data_loaded();

    void write_all(int fd, const char* data, std::size_t len) const;
    void callback_until_success(const EncryptRequest& request) const;
};

RuntimeConfig runtime_config_from_env();

} // namespace dcc
