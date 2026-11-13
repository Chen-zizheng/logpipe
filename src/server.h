#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "file_sink.h"
#include "line_splitter.h"
#include "metrics.h"
#include "platform.h"

struct ServerConfig {
    std::string host = "0.0.0.0";
    int port = 9000;
    std::string out_dir = "logs";
    std::string base_name = "app.log";
    std::uint64_t rotate_bytes = 16ull * 1024 * 1024;
    int keep_files = 5;
    std::string web_dir = "web";
    std::size_t queue_capacity = 8192;
    int writer_threads = 2;
    std::size_t max_line_bytes = 64 * 1024;
    std::size_t tail_lines = 200;
};

class Reactor;

/**
 * 单线程反应堆（Linux epoll / Windows WSAPoll）负责收发，
 * 写盘交给有界队列 + 线程池；队列满时直接丢弃并计数，绝不阻塞 I/O。
 */
class Server {
public:
    explicit Server(ServerConfig cfg);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start();
    void run();
    void stop();

    std::string stats_json() const;
    std::string tail_json(int count) const;
    int port() const { return bound_port_; }

private:
    struct Connection {
        socket_t fd = kInvalidSocket;
        std::string in;
        LineSplitter splitter;
        bool classified = false;
        bool http_mode = false;
    };

    void accept_new();
    void on_readable(socket_t fd);
    void close_conn(socket_t fd);
    void handle_http(socket_t fd, Connection& conn);
    void handle_ingest(Connection& conn);
    void enqueue_line(std::string line);

    bool start_writers();
    void stop_writers();
    void writer_loop();

    std::string read_web_file(const std::string& rel_path) const;

    ServerConfig cfg_;
    Metrics metrics_;

    socket_t listen_fd_ = kInvalidSocket;
    int bound_port_ = 0;
    std::atomic<bool> running_{false};
    std::unique_ptr<Reactor> reactor_;
    std::map<socket_t, Connection> conns_;

    FileSink sink_;
    std::deque<std::string> queue_;
    mutable std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::vector<std::thread> writers_;
    std::atomic<bool> writers_running_{false};

    std::deque<std::string> tail_;
    mutable std::mutex tail_mu_;

    std::chrono::steady_clock::time_point started_at_ = std::chrono::steady_clock::now();
};

/** "9000" -> 9000；非法输入返回 -1。 */
int parse_port(const std::string& text);
