#include "http_server.h"

#include "request.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace dcc {
namespace {

std::string lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

void socket_write_all(int fd, const char* data, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        const ssize_t n = ::send(fd, data + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (n == 0) {
            return;
        }
        off += static_cast<std::size_t>(n);
    }
}

void send_json_response(int fd, int status, const std::string& body) {
    const char* reason = status == 200 ? "OK" : (status == 404 ? "Not Found" : "Internal Server Error");
    const std::string header = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
                               "Content-Type: application/json;charset=UTF-8\r\n"
                               "Content-Length: " + std::to_string(body.size()) + "\r\n"
                               "Connection: close\r\n\r\n";
    socket_write_all(fd, header.data(), header.size());
    socket_write_all(fd, body.data(), body.size());
}

} // namespace

HttpServer::HttpServer(int port, JobScheduler& scheduler) : port_(port), scheduler_(scheduler) {}

void HttpServer::run() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        throw std::runtime_error("socket failed");
    }

    int on = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        throw std::runtime_error(std::string("bind failed: ") + std::strerror(errno));
    }
    if (::listen(listen_fd_, 256) != 0) {
        throw std::runtime_error(std::string("listen failed: ") + std::strerror(errno));
    }

    std::cerr << "HTTP server listening on port " << port_ << "\n";
    while (!stopping_.load()) {
        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (stopping_.load()) {
                break;
            }
            std::cerr << "accept failed: " << std::strerror(errno) << "\n";
            continue;
        }
        std::thread(&HttpServer::handle_client, this, fd).detach();
    }
}

void HttpServer::stop() {
    stopping_.store(true);
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void HttpServer::handle_client(int fd) {
    try {
        std::string req;
        char buf[4096];
        std::size_t header_end = std::string::npos;
        while ((header_end = req.find("\r\n\r\n")) == std::string::npos) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                ::close(fd);
                return;
            }
            req.append(buf, static_cast<std::size_t>(n));
            if (req.size() > 1024 * 1024) {
                throw std::runtime_error("request header too large");
            }
        }

        std::istringstream head(req.substr(0, header_end));
        std::string method;
        std::string path;
        std::string version;
        head >> method >> path >> version;

        std::unordered_map<std::string, std::string> headers;
        std::string line;
        std::getline(head, line);
        while (std::getline(head, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            std::string key = lower_copy(line.substr(0, colon));
            std::string value = line.substr(colon + 1);
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
                value.erase(value.begin());
            }
            headers[key] = value;
        }

        std::size_t content_length = 0;
        auto it = headers.find("content-length");
        if (it != headers.end()) {
            content_length = static_cast<std::size_t>(std::stoull(it->second));
        }
        std::string body = req.substr(header_end + 4);
        while (body.size() < content_length) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            body.append(buf, static_cast<std::size_t>(n));
        }
        if (body.size() > content_length) {
            body.resize(content_length);
        }

        if (method == "GET" && path == "/health") {
            send_json_response(fd, 200, bee_success_json());
            scheduler_.warmup_async();
        } else if (method == "POST" && path == "/encrypt") {
            try {
                scheduler_.enqueue(parse_encrypt_request(body));
                send_json_response(fd, 200, bee_success_json());
            } catch (const std::exception& e) {
                send_json_response(fd, 200, bee_failed_json(e.what()));
            }
        } else {
            send_json_response(fd, 404, bee_failed_json("not found"));
        }
    } catch (const std::exception& e) {
        send_json_response(fd, 500, bee_failed_json(e.what()));
    }
    ::close(fd);
}

} // namespace dcc
