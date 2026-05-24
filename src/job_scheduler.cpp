#include "job_scheduler.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
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
    cfg.tile_rows = getenv_size_or("DCC_TILE_ROWS", 100000);
    cfg.callback_enabled = getenv_or("DCC_DISABLE_CALLBACK", "0") != "1";
    return cfg;
}

JobScheduler::JobScheduler(RuntimeConfig config) : config_(std::move(config)) {
    if (config_.output_dir.empty()) {
        config_.output_dir = "/opt/app/dcc/" + config_.team_code + "/output/";
    }
    config_.job_workers = std::max(1, config_.job_workers);
    config_.compute_threads = std::max(1, config_.compute_threads);
    config_.tile_rows = std::max<std::size_t>(1, config_.tile_rows);
}

JobScheduler::~JobScheduler() {
    stop();
}

void JobScheduler::start() {
    workers_.reserve(static_cast<std::size_t>(config_.job_workers));
    for (int i = 0; i < config_.job_workers; ++i) {
        workers_.emplace_back(&JobScheduler::worker_loop, this, i);
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
}

void JobScheduler::enqueue(EncryptRequest request) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(QueuedJob{request_priority(request), next_sequence_++, std::move(request)});
    }
    cv_.notify_one();
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
        EncryptRequest request;
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
            request = queue_.top().request;
            queue_.pop();
        }
        (void)worker_id;
        process_with_retry(request);
    }
}

void JobScheduler::process_with_retry(const EncryptRequest& request) {
    for (;;) {
        try {
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
    ensure_data_loaded();
    const auto fields = store_.resolve_fields(request.fields);

    Sm4KeySchedule schedule;
    sm4_set_encrypt_key(reinterpret_cast<const unsigned char*>(request.sm4_key.data()), schedule);

    std::filesystem::create_directories(config_.output_dir);
    const std::string final_path = config_.output_dir + "/" + request.request_id + ".csv";
    const std::string tmp_path = final_path + ".tmp";

    const int fd = ::open(tmp_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        throw std::runtime_error("failed to open output file: " + tmp_path + ": " + std::strerror(errno));
    }

    const std::size_t total_rows = store_.row_count();
    const int threads = std::max(1, std::min<int>(config_.compute_threads, static_cast<int>(total_rows == 0 ? 1 : total_rows)));

    try {
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
                write_all(fd, part.data(), part.size());
            }
        }
        if (::close(fd) != 0) {
            throw std::runtime_error("failed to close output file: " + std::string(std::strerror(errno)));
        }
    } catch (...) {
        ::close(fd);
        throw;
    }

    if (::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        throw std::runtime_error("failed to rename output file: " + std::string(std::strerror(errno)));
    }

    std::cerr << "generated " << final_path << "\n";
    callback_until_success(request);
    const auto job_end = std::chrono::steady_clock::now();
    std::cerr << "job complete requestId=" << request.request_id
              << " ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(job_end - job_start).count()
              << "\n";
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
