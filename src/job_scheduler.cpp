#include "job_scheduler.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace dcc {
namespace {

using SteadyTime = std::chrono::steady_clock::time_point;
using WallTime = std::chrono::system_clock::time_point;

std::string getenv_or(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    if (value && *value) {
        return value;
    }
    return fallback;
}

int getenv_int_or(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    try {
        return std::max(1, std::stoi(value));
    } catch (...) {
        return fallback;
    }
}

std::size_t getenv_size_or(const char* name, std::size_t fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    try {
        return std::max<std::size_t>(1, static_cast<std::size_t>(std::stoull(value)));
    } catch (...) {
        return fallback;
    }
}

double elapsed_ms(SteadyTime start, SteadyTime end) {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()) / 1000.0;
}

std::string format_wall_time(WallTime time) {
    const auto since_epoch = time.time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count() % 1000;
    const std::time_t raw = std::chrono::system_clock::to_time_t(time);
    std::tm tm{};
    localtime_r(&raw, &tm);

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setw(3) << std::setfill('0') << millis;
    return out.str();
}

std::mutex& log_mutex() {
    static std::mutex mutex;
    return mutex;
}

void log_timing(const std::string& request_id,
                const std::string& stage,
                WallTime start_wall,
                WallTime end_wall,
                SteadyTime start_steady,
                SteadyTime end_steady,
                const std::string& extra = {}) {
    std::ostringstream line;
    line << "timing requestId=" << request_id
         << " stage=" << stage
         << " start=\"" << format_wall_time(start_wall) << "\""
         << " end=\"" << format_wall_time(end_wall) << "\""
         << " ms=" << std::fixed << std::setprecision(3) << elapsed_ms(start_steady, end_steady);
    if (!extra.empty()) {
        line << ' ' << extra;
    }
    std::lock_guard<std::mutex> lock(log_mutex());
    std::cerr << line.str() << "\n";
}

void log_timing_now(const std::string& request_id, const std::string& stage, const std::string& extra = {}) {
    const auto steady = std::chrono::steady_clock::now();
    const auto wall = std::chrono::system_clock::now();
    log_timing(request_id, stage, wall, wall, steady, steady, extra);
}

std::uint64_t field_priority(std::string_view name) {
    if (name == "device_id" || name == "trans_id") {
        return 64;
    }
    if (name == "business_key") {
        return 40;
    }
    if (name == "user_id" || name == "serial_no" || name == "user_code" || name == "secret_code") {
        return 32;
    }
    if (name == "id_card" || name == "phone" || name == "name" || name == "email") {
        return 8;
    }
    return 16;
}

std::uint64_t request_priority(const EncryptRequest& request) {
    std::uint64_t priority = request.fields.empty() ? 1 : request.fields.size();
    for (const auto& field : request.fields) {
        priority += field_priority(field);
    }
    return priority;
}

struct ParsedUrl {
    std::string host;
    std::string port = "80";
    std::string path = "/";
};

ParsedUrl parse_http_url(const std::string& url) {
    constexpr const char* prefix = "http://";
    if (url.rfind(prefix, 0) != 0) {
        throw std::invalid_argument("only http callback URL is supported");
    }
    ParsedUrl parsed;
    std::size_t pos = std::strlen(prefix);
    std::size_t slash = url.find('/', pos);
    std::string authority = slash == std::string::npos ? url.substr(pos) : url.substr(pos, slash - pos);
    parsed.path = slash == std::string::npos ? "/" : url.substr(slash);
    std::size_t colon = authority.find(':');
    if (colon == std::string::npos) {
        parsed.host = authority;
    } else {
        parsed.host = authority.substr(0, colon);
        parsed.port = authority.substr(colon + 1);
    }
    if (parsed.host.empty()) {
        throw std::invalid_argument("callback URL host is empty");
    }
    return parsed;
}

void send_all_socket(int fd, const char* data, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        const ssize_t n = ::send(fd, data + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("socket send failed: ") + std::strerror(errno));
        }
        if (n == 0) {
            throw std::runtime_error("socket send returned zero");
        }
        off += static_cast<std::size_t>(n);
    }
}

} // namespace

RuntimeConfig runtime_config_from_env() {
    RuntimeConfig cfg;
    cfg.team_code = getenv_or("DCC_TEAM_CODE", "baseline");
    cfg.data_path = getenv_or("DCC_DATA_PATH", "/dcc/root/table_data.csv");
    cfg.output_dir = getenv_or("DCC_OUTPUT_DIR", "/opt/app/dcc/" + cfg.team_code + "/output/");
    cfg.callback_url = getenv_or("DCC_CALLBACK_URL", "http://dcc08-data-encrypt.paas.cmbchina.cn/callback");
    cfg.job_workers = getenv_int_or("DCC_JOB_WORKERS", 4);
    cfg.compute_threads = getenv_int_or("DCC_COMPUTE_THREADS", 1);
    cfg.queue_coalesce_ms = getenv_int_or("DCC_QUEUE_COALESCE_MS", 0);
    cfg.write_workers = getenv_int_or("DCC_WRITE_WORKERS", 4);
    cfg.tile_rows = getenv_size_or("DCC_TILE_ROWS", 100000);
    cfg.early_max_buffered_jobs = getenv_size_or("DCC_EARLY_MAX_BUFFERED_JOBS", 128);
    cfg.callback_enabled = getenv_or("DCC_DISABLE_CALLBACK", "0") != "1";
    cfg.early_callback = getenv_or("DCC_EARLY_CALLBACK", "1") != "0";
    return cfg;
}

JobScheduler::JobScheduler(RuntimeConfig config) : config_(std::move(config)) {
    if (config_.output_dir.empty()) {
        config_.output_dir = "/opt/app/dcc/" + config_.team_code + "/output/";
    }
    config_.job_workers = std::max(1, config_.job_workers);
    config_.compute_threads = std::max(1, config_.compute_threads);
    config_.write_workers = std::max(1, config_.write_workers);
    config_.tile_rows = std::max<std::size_t>(1, config_.tile_rows);
    config_.early_max_buffered_jobs = std::max<std::size_t>(1, config_.early_max_buffered_jobs);
}

JobScheduler::~JobScheduler() {
    stop();
}

void JobScheduler::start() {
    workers_.reserve(static_cast<std::size_t>(config_.job_workers));
    for (int i = 0; i < config_.job_workers; ++i) {
        workers_.emplace_back(&JobScheduler::worker_loop, this, i);
    }
    if (config_.early_callback && config_.callback_enabled) {
        write_workers_.reserve(static_cast<std::size_t>(config_.write_workers));
        for (int i = 0; i < config_.write_workers; ++i) {
            write_workers_.emplace_back(&JobScheduler::writer_loop, this, i);
        }
    }
}

void JobScheduler::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    if (warmup_thread_.joinable()) {
        warmup_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        writer_stopping_ = true;
    }
    write_cv_.notify_all();
    write_space_cv_.notify_all();
    for (auto& worker : write_workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    write_workers_.clear();
}

void JobScheduler::enqueue(EncryptRequest request) {
    const auto enqueue_start_steady = std::chrono::steady_clock::now();
    const auto enqueue_start_wall = std::chrono::system_clock::now();
    const std::uint64_t priority = request_priority(request);
    std::size_t queue_depth = 0;
    const std::string request_id = request.request_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(QueuedJob{priority, next_sequence_++, enqueue_start_steady, enqueue_start_wall, std::move(request)});
        queue_depth = queue_.size();
    }
    cv_.notify_one();
    const auto enqueue_end_steady = std::chrono::steady_clock::now();
    const auto enqueue_end_wall = std::chrono::system_clock::now();
    log_timing(request_id, "enqueue",
               enqueue_start_wall, enqueue_end_wall,
               enqueue_start_steady, enqueue_end_steady,
               "priority=" + std::to_string(priority) +
                   " queueDepth=" + std::to_string(queue_depth));
}

void JobScheduler::warmup_async() {
    std::call_once(warmup_once_, [&] {
        warmup_thread_ = std::thread([this] {
            try {
                ensure_data_loaded();
            } catch (const std::exception& e) {
                std::cerr << "warmup failed: " << e.what() << "\n";
            }
        });
    });
}

void JobScheduler::worker_loop(int worker_id) {
    while (true) {
        QueuedJob job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) {
                return;
            }
            if (config_.queue_coalesce_ms > 0 && queue_.size() < static_cast<std::size_t>(config_.job_workers * 2)) {
                cv_.wait_for(lock, std::chrono::milliseconds(config_.queue_coalesce_ms), [&] {
                    return stopping_;
                });
                if (stopping_ && queue_.empty()) {
                    return;
                }
                if (queue_.empty()) {
                    continue;
                }
            }
            job = queue_.top();
            queue_.pop();
        }
        const auto dequeue_steady = std::chrono::steady_clock::now();
        const auto dequeue_wall = std::chrono::system_clock::now();
        log_timing(job.request.request_id, "queue_wait",
                   job.enqueue_wall, dequeue_wall,
                   job.enqueue_steady, dequeue_steady,
                   "worker=" + std::to_string(worker_id));
        process_with_retry(job.request, worker_id);
    }
}

void JobScheduler::writer_loop(int writer_id) {
    while (true) {
        PendingWrite write;
        {
            std::unique_lock<std::mutex> lock(write_mutex_);
            write_cv_.wait(lock, [&] { return writer_stopping_ || !write_queue_.empty(); });
            if (write_queue_.empty()) {
                return;
            }
            write = std::move(write_queue_.front());
            write_queue_.pop_front();
        }
        write_space_cv_.notify_one();

        for (;;) {
            try {
                const auto dequeue_steady = std::chrono::steady_clock::now();
                const auto dequeue_wall = std::chrono::system_clock::now();
                log_timing(write.request_id, "write_queue_wait",
                           write.enqueue_wall, dequeue_wall,
                           write.enqueue_steady, dequeue_steady,
                           "writer=" + std::to_string(writer_id) +
                               " bytes=" + std::to_string(write.data.size()));
                write_output_file(write, writer_id);
                std::cerr << "writer " << writer_id << " generated " << write.final_path << "\n";
                break;
            } catch (const std::exception& e) {
                std::cerr << "write failed, will retry requestId=" << write.request_id
                          << " error=" << e.what() << "\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
}

void JobScheduler::process_with_retry(const EncryptRequest& request, int worker_id) {
    for (;;) {
        try {
            log_timing_now(request.request_id, "worker_start", "worker=" + std::to_string(worker_id));
            process_once(request);
            return;
        } catch (const std::exception& e) {
            std::cerr << "job failed, will retry requestId=" << request.request_id << " error=" << e.what() << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void JobScheduler::ensure_data_loaded() {
    std::call_once(load_once_, [&] {
        const auto start = std::chrono::steady_clock::now();
        store_.load(config_.data_path);
        const auto end = std::chrono::steady_clock::now();
        std::cerr << "loaded CSV rows=" << store_.row_count()
                  << " in " << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count()
                  << " ms\n";
    });
}

void JobScheduler::process_once(const EncryptRequest& request) {
    const auto job_start = std::chrono::steady_clock::now();
    const auto job_start_wall = std::chrono::system_clock::now();

    auto stage_start = std::chrono::steady_clock::now();
    auto stage_start_wall = std::chrono::system_clock::now();
    ensure_data_loaded();
    auto stage_end = std::chrono::steady_clock::now();
    auto stage_end_wall = std::chrono::system_clock::now();
    log_timing(request.request_id, "ensure_data_loaded",
               stage_start_wall, stage_end_wall, stage_start, stage_end,
               "rows=" + std::to_string(store_.row_count()));

    stage_start = std::chrono::steady_clock::now();
    stage_start_wall = std::chrono::system_clock::now();
    const auto fields = store_.resolve_fields(request.fields);
    stage_end = std::chrono::steady_clock::now();
    stage_end_wall = std::chrono::system_clock::now();
    log_timing(request.request_id, "resolve_fields",
               stage_start_wall, stage_end_wall, stage_start, stage_end,
               "fieldCount=" + std::to_string(fields.size()));

    Sm4KeySchedule schedule;
    stage_start = std::chrono::steady_clock::now();
    stage_start_wall = std::chrono::system_clock::now();
    sm4_set_encrypt_key(reinterpret_cast<const unsigned char*>(request.sm4_key.data()), schedule);
    stage_end = std::chrono::steady_clock::now();
    stage_end_wall = std::chrono::system_clock::now();
    log_timing(request.request_id, "sm4_key_schedule",
               stage_start_wall, stage_end_wall, stage_start, stage_end);

    stage_start = std::chrono::steady_clock::now();
    stage_start_wall = std::chrono::system_clock::now();
    std::filesystem::create_directories(config_.output_dir);
    stage_end = std::chrono::steady_clock::now();
    stage_end_wall = std::chrono::system_clock::now();
    log_timing(request.request_id, "create_output_dir",
               stage_start_wall, stage_end_wall, stage_start, stage_end,
               "dir=" + config_.output_dir);

    const std::string final_path = config_.output_dir + "/" + request.request_id + ".csv";
    const std::string tmp_path = final_path + ".tmp";

    if (config_.early_callback && config_.callback_enabled) {
        stage_start = std::chrono::steady_clock::now();
        stage_start_wall = std::chrono::system_clock::now();
        std::string rendered = render_to_memory(fields, schedule);
        stage_end = std::chrono::steady_clock::now();
        stage_end_wall = std::chrono::system_clock::now();
        const std::size_t rendered_bytes = rendered.size();
        log_timing(request.request_id, "render_to_memory",
                   stage_start_wall, stage_end_wall, stage_start, stage_end,
                   "rows=" + std::to_string(store_.row_count()) +
                       " bytes=" + std::to_string(rendered_bytes) +
                       " fieldCount=" + std::to_string(fields.size()));

        stage_start = std::chrono::steady_clock::now();
        stage_start_wall = std::chrono::system_clock::now();
        callback_until_success(request);
        stage_end = std::chrono::steady_clock::now();
        stage_end_wall = std::chrono::system_clock::now();
        log_timing(request.request_id, "callback",
                   stage_start_wall, stage_end_wall, stage_start, stage_end,
                   "url=" + config_.callback_url);

        stage_start = std::chrono::steady_clock::now();
        stage_start_wall = std::chrono::system_clock::now();
        enqueue_write(PendingWrite{request.request_id, final_path, tmp_path, std::move(rendered), {}, {}});
        stage_end = std::chrono::steady_clock::now();
        stage_end_wall = std::chrono::system_clock::now();
        log_timing(request.request_id, "enqueue_background_write",
                   stage_start_wall, stage_end_wall, stage_start, stage_end,
                   "bytes=" + std::to_string(rendered_bytes));
        const auto job_end = std::chrono::steady_clock::now();
        const auto job_end_wall = std::chrono::system_clock::now();
        std::cerr << "job callback-complete requestId=" << request.request_id
                  << " ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(job_end - job_start).count()
                  << "\n";
        log_timing(request.request_id, "job_callback_path_total",
                   job_start_wall, job_end_wall, job_start, job_end,
                   "rows=" + std::to_string(store_.row_count()) +
                       " bytes=" + std::to_string(rendered_bytes) +
                       " workerPath=early_callback");
        return;
    }

    stage_start = std::chrono::steady_clock::now();
    stage_start_wall = std::chrono::system_clock::now();
    const int fd = ::open(tmp_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    stage_end = std::chrono::steady_clock::now();
    stage_end_wall = std::chrono::system_clock::now();
    if (fd < 0) {
        throw std::runtime_error("failed to open output file: " + tmp_path + ": " + std::strerror(errno));
    }
    log_timing(request.request_id, "file_open",
               stage_start_wall, stage_end_wall, stage_start, stage_end,
               "path=" + tmp_path);

    const std::size_t total_rows = store_.row_count();
    const int threads = std::max(1, std::min<int>(config_.compute_threads, static_cast<int>(total_rows == 0 ? 1 : total_rows)));
    std::size_t rendered_bytes = 0;

    try {
        stage_start = std::chrono::steady_clock::now();
        stage_start_wall = std::chrono::system_clock::now();
        for (std::size_t tile_begin = 0; tile_begin < total_rows; tile_begin += config_.tile_rows) {
            const std::size_t tile_end = std::min(total_rows, tile_begin + config_.tile_rows);
            const std::size_t tile_count = tile_end - tile_begin;
            const int active_threads = std::max(1, std::min<int>(threads, static_cast<int>(tile_count)));

            std::vector<std::string> parts(static_cast<std::size_t>(active_threads));
            std::vector<std::thread> workers;
            workers.reserve(static_cast<std::size_t>(active_threads - 1));

            auto render_part = [&](int tid) {
                const std::size_t begin = tile_begin + (tile_count * static_cast<std::size_t>(tid)) / active_threads;
                const std::size_t end = tile_begin + (tile_count * static_cast<std::size_t>(tid + 1)) / active_threads;
                std::string& part = parts[static_cast<std::size_t>(tid)];
                part.reserve((end - begin) * std::max<std::size_t>(64, fields.size() * 48));
                for (std::size_t row = begin; row < end; ++row) {
                    store_.append_rendered_row(row, fields, schedule, part);
                }
            };

            for (int tid = 1; tid < active_threads; ++tid) {
                workers.emplace_back(render_part, tid);
            }
            render_part(0);
            for (auto& worker : workers) {
                worker.join();
            }
            for (const auto& part : parts) {
                rendered_bytes += part.size();
                write_all(fd, part.data(), part.size());
            }
        }
        stage_end = std::chrono::steady_clock::now();
        stage_end_wall = std::chrono::system_clock::now();
        log_timing(request.request_id, "render_and_write_tiles",
                   stage_start_wall, stage_end_wall, stage_start, stage_end,
                   "rows=" + std::to_string(total_rows) +
                       " bytes=" + std::to_string(rendered_bytes) +
                       " fieldCount=" + std::to_string(fields.size()) +
                       " computeThreads=" + std::to_string(threads));

        stage_start = std::chrono::steady_clock::now();
        stage_start_wall = std::chrono::system_clock::now();
        if (::close(fd) != 0) {
            throw std::runtime_error("failed to close output file: " + std::string(std::strerror(errno)));
        }
        stage_end = std::chrono::steady_clock::now();
        stage_end_wall = std::chrono::system_clock::now();
        log_timing(request.request_id, "file_close",
                   stage_start_wall, stage_end_wall, stage_start, stage_end,
                   "path=" + tmp_path);
    } catch (...) {
        ::close(fd);
        throw;
    }

    stage_start = std::chrono::steady_clock::now();
    stage_start_wall = std::chrono::system_clock::now();
    if (::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        throw std::runtime_error("failed to rename output file: " + std::string(std::strerror(errno)));
    }
    stage_end = std::chrono::steady_clock::now();
    stage_end_wall = std::chrono::system_clock::now();
    log_timing(request.request_id, "file_rename",
               stage_start_wall, stage_end_wall, stage_start, stage_end,
               "finalPath=" + final_path);

    std::cerr << "generated " << final_path << "\n";
    stage_start = std::chrono::steady_clock::now();
    stage_start_wall = std::chrono::system_clock::now();
    callback_until_success(request);
    stage_end = std::chrono::steady_clock::now();
    stage_end_wall = std::chrono::system_clock::now();
    log_timing(request.request_id, "callback",
               stage_start_wall, stage_end_wall, stage_start, stage_end,
               "url=" + config_.callback_url);
    const auto job_end = std::chrono::steady_clock::now();
    const auto job_end_wall = std::chrono::system_clock::now();
    std::cerr << "job complete requestId=" << request.request_id
              << " ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(job_end - job_start).count()
              << "\n";
    log_timing(request.request_id, "job_conservative_path_total",
               job_start_wall, job_end_wall, job_start, job_end,
               "rows=" + std::to_string(total_rows) +
                   " bytes=" + std::to_string(rendered_bytes) +
                   " workerPath=conservative");
}

std::string JobScheduler::render_to_memory(const std::vector<FieldRef>& fields, const Sm4KeySchedule& schedule) const {
    const std::size_t total_rows = store_.row_count();
    const int threads = std::max(1, std::min<int>(config_.compute_threads, static_cast<int>(total_rows == 0 ? 1 : total_rows)));

    std::string rendered;
    rendered.reserve(total_rows * std::max<std::size_t>(64, fields.size() * 48));

    for (std::size_t tile_begin = 0; tile_begin < total_rows; tile_begin += config_.tile_rows) {
        const std::size_t tile_end = std::min(total_rows, tile_begin + config_.tile_rows);
        const std::size_t tile_count = tile_end - tile_begin;
        const int active_threads = std::max(1, std::min<int>(threads, static_cast<int>(tile_count)));

        std::vector<std::string> parts(static_cast<std::size_t>(active_threads));
        std::vector<std::thread> workers;
        workers.reserve(static_cast<std::size_t>(active_threads - 1));

        auto render_part = [&](int tid) {
            const std::size_t begin = tile_begin + (tile_count * static_cast<std::size_t>(tid)) / active_threads;
            const std::size_t end = tile_begin + (tile_count * static_cast<std::size_t>(tid + 1)) / active_threads;
            std::string& part = parts[static_cast<std::size_t>(tid)];
            part.reserve((end - begin) * std::max<std::size_t>(64, fields.size() * 48));
            for (std::size_t row = begin; row < end; ++row) {
                store_.append_rendered_row(row, fields, schedule, part);
            }
        };

        for (int tid = 1; tid < active_threads; ++tid) {
            workers.emplace_back(render_part, tid);
        }
        render_part(0);
        for (auto& worker : workers) {
            worker.join();
        }
        for (const auto& part : parts) {
            rendered.append(part);
        }
    }
    return rendered;
}

void JobScheduler::enqueue_write(PendingWrite write) {
    {
        std::unique_lock<std::mutex> lock(write_mutex_);
        write_space_cv_.wait(lock, [&] {
            return writer_stopping_ || write_queue_.size() < config_.early_max_buffered_jobs;
        });
        if (writer_stopping_) {
            throw std::runtime_error("writer queue stopped");
        }
        write.enqueue_steady = std::chrono::steady_clock::now();
        write.enqueue_wall = std::chrono::system_clock::now();
        write_queue_.push_back(std::move(write));
    }
    write_cv_.notify_one();
}

void JobScheduler::write_output_file(const PendingWrite& write, int writer_id) const {
    const auto total_start_steady = std::chrono::steady_clock::now();
    const auto total_start_wall = std::chrono::system_clock::now();

    auto stage_start = std::chrono::steady_clock::now();
    auto stage_start_wall = std::chrono::system_clock::now();
    std::filesystem::create_directories(config_.output_dir);
    auto stage_end = std::chrono::steady_clock::now();
    auto stage_end_wall = std::chrono::system_clock::now();
    log_timing(write.request_id, "writer_create_output_dir",
               stage_start_wall, stage_end_wall, stage_start, stage_end,
               "writer=" + std::to_string(writer_id) +
                   " dir=" + config_.output_dir);

    stage_start = std::chrono::steady_clock::now();
    stage_start_wall = std::chrono::system_clock::now();
    const int fd = ::open(write.tmp_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    stage_end = std::chrono::steady_clock::now();
    stage_end_wall = std::chrono::system_clock::now();
    if (fd < 0) {
        throw std::runtime_error("failed to open output file: " + write.tmp_path + ": " + std::strerror(errno));
    }
    log_timing(write.request_id, "writer_file_open",
               stage_start_wall, stage_end_wall, stage_start, stage_end,
               "writer=" + std::to_string(writer_id) +
                   " path=" + write.tmp_path);

    try {
        stage_start = std::chrono::steady_clock::now();
        stage_start_wall = std::chrono::system_clock::now();
        write_all(fd, write.data.data(), write.data.size());
        stage_end = std::chrono::steady_clock::now();
        stage_end_wall = std::chrono::system_clock::now();
        log_timing(write.request_id, "writer_file_write",
                   stage_start_wall, stage_end_wall, stage_start, stage_end,
                   "writer=" + std::to_string(writer_id) +
                       " bytes=" + std::to_string(write.data.size()));

        stage_start = std::chrono::steady_clock::now();
        stage_start_wall = std::chrono::system_clock::now();
        if (::close(fd) != 0) {
            throw std::runtime_error("failed to close output file: " + std::string(std::strerror(errno)));
        }
        stage_end = std::chrono::steady_clock::now();
        stage_end_wall = std::chrono::system_clock::now();
        log_timing(write.request_id, "writer_file_close",
                   stage_start_wall, stage_end_wall, stage_start, stage_end,
                   "writer=" + std::to_string(writer_id) +
                       " path=" + write.tmp_path);
    } catch (...) {
        ::close(fd);
        throw;
    }

    stage_start = std::chrono::steady_clock::now();
    stage_start_wall = std::chrono::system_clock::now();
    if (::rename(write.tmp_path.c_str(), write.final_path.c_str()) != 0) {
        throw std::runtime_error("failed to rename output file: " + std::string(std::strerror(errno)));
    }
    stage_end = std::chrono::steady_clock::now();
    stage_end_wall = std::chrono::system_clock::now();
    log_timing(write.request_id, "writer_file_rename",
               stage_start_wall, stage_end_wall, stage_start, stage_end,
               "writer=" + std::to_string(writer_id) +
                   " finalPath=" + write.final_path);

    const auto total_end_steady = std::chrono::steady_clock::now();
    const auto total_end_wall = std::chrono::system_clock::now();
    log_timing(write.request_id, "writer_total",
               total_start_wall, total_end_wall, total_start_steady, total_end_steady,
               "writer=" + std::to_string(writer_id) +
                   " bytes=" + std::to_string(write.data.size()));
}

void JobScheduler::write_all(int fd, const char* data, std::size_t len) const {
    std::size_t off = 0;
    while (off < len) {
        const ssize_t n = ::write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("write failed: ") + std::strerror(errno));
        }
        if (n == 0) {
            throw std::runtime_error("write returned zero");
        }
        off += static_cast<std::size_t>(n);
    }
}

void JobScheduler::callback_until_success(const EncryptRequest& request) const {
    if (!config_.callback_enabled) {
        std::cerr << "callback disabled requestId=" << request.request_id << "\n";
        return;
    }

    const ParsedUrl url = parse_http_url(config_.callback_url);
    const std::string body = "{\"teamCode\":\"" + json_escape(config_.team_code) +
                             "\",\"requestId\":\"" + json_escape(request.request_id) +
                             "\",\"ip\":\"" + json_escape(request.ip) + "\"}";
    const std::string http = "POST " + url.path + " HTTP/1.1\r\n"
                             "Host: " + url.host + "\r\n"
                             "Content-Type: application/json\r\n"
                             "Content-Length: " + std::to_string(body.size()) + "\r\n"
                             "Connection: close\r\n\r\n" + body;

    for (;;) {
        try {
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* result = nullptr;
            const int gai = ::getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &result);
            if (gai != 0) {
                throw std::runtime_error(std::string("getaddrinfo failed: ") + gai_strerror(gai));
            }

            int fd = -1;
            for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
                fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
                if (fd < 0) {
                    continue;
                }
                if (::connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
                    break;
                }
                ::close(fd);
                fd = -1;
            }
            ::freeaddrinfo(result);
            if (fd < 0) {
                throw std::runtime_error("callback connect failed");
            }

            send_all_socket(fd, http.data(), http.size());
            char buf[256];
            (void)::recv(fd, buf, sizeof(buf), 0);
            ::close(fd);
            std::cerr << "callback sent requestId=" << request.request_id << "\n";
            return;
        } catch (const std::exception& e) {
            std::cerr << "callback failed requestId=" << request.request_id << " error=" << e.what() << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

} // namespace dcc
