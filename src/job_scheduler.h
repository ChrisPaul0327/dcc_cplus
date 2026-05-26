#pragma once

#include "data_store.h"
#include "request.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
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
    int write_workers = 4;
    std::size_t tile_rows = 100000;
    std::size_t early_max_buffered_jobs = 128;
    bool callback_enabled = true;
    bool early_callback = true;
};

struct QueuedJob {
    std::uint64_t priority = 0;
    std::uint64_t sequence = 0;
    std::chrono::steady_clock::time_point enqueue_steady;
    std::chrono::system_clock::time_point enqueue_wall;
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

    struct PendingWrite {
        std::string request_id;
        std::string final_path;
        std::string tmp_path;
        std::string data;
        std::chrono::steady_clock::time_point enqueue_steady;
        std::chrono::system_clock::time_point enqueue_wall;
    };

    std::mutex write_mutex_;
    std::condition_variable write_cv_;
    std::condition_variable write_space_cv_;
    std::deque<PendingWrite> write_queue_;
    bool writer_stopping_ = false;
    std::vector<std::thread> write_workers_;

    void worker_loop(int worker_id);
    void writer_loop(int writer_id);
    void process_with_retry(const EncryptRequest& request, int worker_id);
    void process_once(const EncryptRequest& request);
    void ensure_data_loaded();

    std::string render_to_memory(const std::vector<FieldRef>& fields, const Sm4KeySchedule& schedule) const;
    void enqueue_write(PendingWrite write);
    void write_output_file(const PendingWrite& write, int writer_id) const;
    void write_all(int fd, const char* data, std::size_t len) const;
    void callback_until_success(const EncryptRequest& request) const;
};

RuntimeConfig runtime_config_from_env();

} // namespace dcc
