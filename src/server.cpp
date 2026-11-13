#include "server.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include "http.h"

#ifdef _WIN32
// WSAPoll 在 winsock2.h 里，platform.h 已经包含
#else
#include <sys/epoll.h>
#endif

namespace {

constexpr int kPollTimeoutMs = 200;
constexpr std::size_t kReadChunk = 16 * 1024;
constexpr std::size_t kMaxHttpHeader = 16 * 1024;

bool looks_like_http(const std::string& buffer) {
    return buffer.rfind("GET ", 0) == 0 || buffer.rfind("HEAD ", 0) == 0 ||
           buffer.rfind("POST ", 0) == 0 || buffer.rfind("PUT ", 0) == 0;
}

std::string escape_json(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

bool send_all(socket_t fd, const std::string& data) {
    std::size_t sent = 0;
    int retries = 0;
    while (sent < data.size()) {
        const int n = sock_write(fd, data.data() + sent, static_cast<int>(data.size() - sent));
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && sock_would_block() && retries++ < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------- reactor ---

class Reactor {
public:
    Reactor() = default;
    ~Reactor();

    bool init();
    bool add(socket_t fd);
    void del(socket_t fd);
    int wait(std::vector<socket_t>& readable, int timeout_ms);

private:
#ifdef _WIN32
    std::vector<WSAPOLLFD> entries_;
#else
    int epfd_ = -1;
#endif
};

#ifdef _WIN32

Reactor::~Reactor() = default;

bool Reactor::init() { return true; }

bool Reactor::add(socket_t fd) {
    WSAPOLLFD entry{};
    entry.fd = fd;
    entry.events = POLLRDNORM;
    entries_.push_back(entry);
    return true;
}

void Reactor::del(socket_t fd) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [fd](const WSAPOLLFD& e) { return e.fd == fd; }),
                   entries_.end());
}

int Reactor::wait(std::vector<socket_t>& readable, int timeout_ms) {
    if (entries_.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        return 0;
    }
    const int rc = ::WSAPoll(entries_.data(), static_cast<ULONG>(entries_.size()), timeout_ms);
    if (rc <= 0) return rc;
    readable.clear();
    for (const auto& e : entries_) {
        if (e.revents & (POLLRDNORM | POLLERR | POLLHUP | POLLNVAL)) readable.push_back(e.fd);
    }
    return rc;
}

#else

Reactor::~Reactor() {
    if (epfd_ >= 0) ::close(epfd_);
}

bool Reactor::init() {
    epfd_ = ::epoll_create1(0);
    return epfd_ >= 0;
}

bool Reactor::add(socket_t fd) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    return ::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) == 0;
}

void Reactor::del(socket_t fd) {
    if (epfd_ >= 0) ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
}

int Reactor::wait(std::vector<socket_t>& readable, int timeout_ms) {
    epoll_event events[256];
    const int rc = ::epoll_wait(epfd_, events, 256, timeout_ms);
    if (rc <= 0) return rc;
    readable.clear();
    for (int i = 0; i < rc; ++i) readable.push_back(events[i].data.fd);
    return rc;
}

#endif

// ----------------------------------------------------------------- server ---

int parse_port(const std::string& text) {
    if (text.empty()) return -1;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') return -1;
    if (value < 0 || value > 65535) return -1;
    return static_cast<int>(value);
}

Server::Server(ServerConfig cfg)
    : cfg_(std::move(cfg)),
      reactor_(std::make_unique<Reactor>()),
      sink_(cfg_.out_dir, cfg_.base_name, cfg_.rotate_bytes, cfg_.keep_files) {}

Server::~Server() { stop(); }

bool Server::start() {
    if (!net_init()) {
        std::fprintf(stderr, "logpipe: winsock init failed\n");
        return false;
    }
    if (!reactor_->init()) {
        std::fprintf(stderr, "logpipe: reactor init failed\n");
        return false;
    }
    if (!sink_.open()) {
        std::fprintf(stderr, "logpipe: cannot open sink in %s\n", cfg_.out_dir.c_str());
        return false;
    }

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd_ == kInvalidSocket) {
        std::fprintf(stderr, "logpipe: socket() failed (%d)\n", sock_last_error());
        return false;
    }
    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(cfg_.port));
    if (cfg_.host.empty() || cfg_.host == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, cfg_.host.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "logpipe: bad host %s\n", cfg_.host.c_str());
        return false;
    }

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "logpipe: bind %s:%d failed (%d)\n", cfg_.host.c_str(), cfg_.port,
                     sock_last_error());
        return false;
    }
    if (::listen(listen_fd_, 128) != 0) {
        std::fprintf(stderr, "logpipe: listen failed (%d)\n", sock_last_error());
        return false;
    }

    sockaddr_in bound{};
#ifdef _WIN32
    int bound_len = sizeof(bound);
#else
    socklen_t bound_len = sizeof(bound);
#endif
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
        bound_port_ = ntohs(bound.sin_port);
    } else {
        bound_port_ = cfg_.port;
    }

    if (!sock_set_nonblocking(listen_fd_)) {
        std::fprintf(stderr, "logpipe: cannot set non-blocking listen socket\n");
        return false;
    }
    if (!reactor_->add(listen_fd_)) {
        std::fprintf(stderr, "logpipe: cannot register listen socket\n");
        return false;
    }

    start_writers();
    running_ = true;
    std::printf("logpipe: listening on %s:%d -> %s\n", cfg_.host.c_str(), bound_port_,
                sink_.current_path().c_str());
    std::fflush(stdout);
    return true;
}

void Server::run() {
    std::vector<socket_t> readable;
    while (running_.load()) {
        const int rc = reactor_->wait(readable, kPollTimeoutMs);
        if (rc < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            break;
        }
        for (socket_t fd : readable) {
            if (!running_.load()) break;
            if (fd == listen_fd_) {
                accept_new();
            } else if (conns_.count(fd) != 0) {
                on_readable(fd);
            }
        }
    }
}

void Server::stop() {
    if (running_.exchange(false)) {
        // 已经在跑：正常关闭
    }
    if (listen_fd_ != kInvalidSocket) {
        sock_close(listen_fd_);
        listen_fd_ = kInvalidSocket;
    }
    for (auto& pair : conns_) sock_close(pair.first);
    conns_.clear();
    stop_writers();
    sink_.flush();
    sink_.close();
}

void Server::accept_new() {
    for (;;) {
        socket_t fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd == kInvalidSocket) return;
        if (!sock_set_nonblocking(fd)) {
            sock_close(fd);
            continue;
        }
        int yes = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));

        Connection conn;
        conn.fd = fd;
        conn.splitter = LineSplitter(cfg_.max_line_bytes);
        conns_[fd] = std::move(conn);
        if (!reactor_->add(fd)) {
            conns_.erase(fd);
            sock_close(fd);
            continue;
        }
        ++metrics_.accepted;
    }
}

void Server::close_conn(socket_t fd) {
    reactor_->del(fd);
    sock_close(fd);
    conns_.erase(fd);
    ++metrics_.closed;
}

void Server::on_readable(socket_t fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    Connection& conn = it->second;

    char buffer[kReadChunk];
    for (;;) {
        const int n = sock_read(fd, buffer, static_cast<int>(sizeof(buffer)));
        if (n > 0) {
            conn.in.append(buffer, static_cast<std::size_t>(n));
            metrics_.bytes_in += static_cast<std::uint64_t>(n);
            continue;
        }
        if (n == 0) {
            // 对端关闭：如果还有半行，按最后一行处理
            if (!conn.in.empty() && !conn.http_mode) handle_ingest(conn);
            close_conn(fd);
            return;
        }
        if (sock_would_block()) break;
        close_conn(fd);
        return;
    }

    if (conn.in.empty()) return;

    if (!conn.classified) {
        conn.classified = true;
        conn.http_mode = looks_like_http(conn.in);
    }

    if (conn.http_mode) {
        handle_http(fd, conn);
    } else {
        handle_ingest(conn);
    }
}

void Server::handle_ingest(Connection& conn) {
    if (conn.in.empty()) return;
    LineSplitter::LineHandler handler = [this](std::string_view line) {
        enqueue_line(std::string(line));
    };
    conn.splitter.feed(conn.in.data(), conn.in.size(), handler);
    conn.in.clear();
}

void Server::handle_http(socket_t fd, Connection& conn) {
    if (conn.in.size() > kMaxHttpHeader) {
        send_all(fd, http_response(413, "text/plain; charset=utf-8", "request too large\n"));
        close_conn(fd);
        return;
    }
    if (!http_request_complete(conn.in)) return;  // 等剩下的包头

    HttpRequest req;
    if (!http_parse_request(conn.in, req)) {
        send_all(fd, http_response(400, "text/plain; charset=utf-8", "bad request\n"));
        close_conn(fd);
        return;
    }
    ++metrics_.http_requests;

    std::string status_line;
    if (req.path == "/api/stats") {
        send_all(fd, http_response(200, "application/json; charset=utf-8", stats_json()));
    } else if (req.path == "/api/tail") {
        const int n = http_query_int(req.query, "n", 50, 1, 200);
        send_all(fd, http_response(200, "application/json; charset=utf-8", tail_json(n)));
    } else if (req.method != "GET" && req.method != "HEAD") {
        send_all(fd, http_response(405, "text/plain; charset=utf-8", "method not allowed\n"));
    } else {
        std::string rel = req.path == "/" ? "index.html" : req.path.substr(1);
        // 目录穿越保护
        if (rel.find("..") != std::string::npos) {
            send_all(fd, http_response(400, "text/plain; charset=utf-8", "bad path\n"));
        } else {
            const std::string body = read_web_file(rel);
            if (body.empty()) {
                send_all(fd, http_response(404, "text/plain; charset=utf-8", "not found\n"));
            } else {
                send_all(fd, http_response(200, http_content_type_for(rel), body));
            }
        }
    }
    close_conn(fd);
}

std::string Server::read_web_file(const std::string& rel_path) const {
    const std::filesystem::path path = std::filesystem::path(cfg_.web_dir) / rel_path;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return {};
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}

void Server::enqueue_line(std::string line) {
    {
        std::lock_guard<std::mutex> lock(tail_mu_);
        tail_.push_back(line);
        while (tail_.size() > cfg_.tail_lines) tail_.pop_front();
    }
    ++metrics_.records;

    std::lock_guard<std::mutex> lock(queue_mu_);
    if (queue_.size() >= cfg_.queue_capacity) {
        ++metrics_.dropped;
        return;
    }
    queue_.push_back(std::move(line));
    queue_cv_.notify_one();
}

bool Server::start_writers() {
    writers_running_ = true;
    const int n = cfg_.writer_threads <= 0 ? 1 : cfg_.writer_threads;
    for (int i = 0; i < n; ++i) writers_.emplace_back([this] { writer_loop(); });
    return true;
}

void Server::stop_writers() {
    if (!writers_running_.exchange(false)) return;
    queue_cv_.notify_all();
    for (auto& t : writers_) {
        if (t.joinable()) t.join();
    }
    writers_.clear();
}

void Server::writer_loop() {
    // 一次取一批再写：每行一次 write 会让锁和系统调用成为瓶颈
    constexpr std::size_t kBatchBytes = 256 * 1024;
    constexpr std::size_t kBatchLines = 512;
    std::string batch;
    batch.reserve(kBatchBytes + 128);

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(queue_mu_);
            queue_cv_.wait(lock, [this] { return !queue_.empty() || !writers_running_.load(); });
            if (queue_.empty()) {
                if (!writers_running_.load()) return;
                continue;
            }
            std::size_t lines = 0;
            while (!queue_.empty() && batch.size() < kBatchBytes && lines < kBatchLines) {
                batch += queue_.front();
                batch.push_back('\n');
                queue_.pop_front();
                ++lines;
            }
            if (lines >= kBatchLines / 2) queue_cv_.notify_all();
        }
        if (!batch.empty()) {
            if (sink_.write(batch)) {
                metrics_.written_bytes += batch.size();
                // 每批落盘一次：批量已经摊薄了系统调用，flush 后 tail -f 才是实时的
                sink_.flush();
            }
            batch.clear();
        }
    }
}

std::string Server::stats_json() const {
    const std::size_t depth = [this] {
        std::lock_guard<std::mutex> lock(queue_mu_);
        return queue_.size();
    }();

    const auto uptime = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started_at_)
                            .count();

    std::ostringstream os;
    os << '{' << "\"server\":\"logpipe\","
       << "\"port\":" << bound_port_ << ','
       << "\"sink\":\"" << escape_json(sink_.current_path()) << "\","
       << "\"sink_bytes\":" << sink_.bytes_written() << ','
       << "\"rotations\":" << sink_.rotations() << ','
       << "\"uptime_ms\":" << uptime << ','
       << "\"queue_depth\":" << depth << ','
       << "\"queue_capacity\":" << cfg_.queue_capacity << ','
       << "\"metrics\":" << metrics_.to_json() << '}';
    return os.str();
}

std::string Server::tail_json(int count) const {
    std::lock_guard<std::mutex> lock(tail_mu_);
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(count), tail_.size());
    std::ostringstream os;
    os << "{\"lines\":[";
    for (std::size_t i = tail_.size() - n; i < tail_.size(); ++i) {
        if (i != tail_.size() - n) os << ',';
        os << '"' << escape_json(tail_[i]) << '"';
    }
    os << "]}";
    return os.str();
}
