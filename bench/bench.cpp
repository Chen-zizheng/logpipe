// 压测客户端：开 N 条连接，每条连接发 M 行，统计吞吐与耗时。
#include <algorithm>
#include <atomic>
#include <climits>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "platform.h"

namespace {

struct Options {
    std::string host = "127.0.0.1";
    int port = 9000;
    int connections = 8;
    int lines_per_conn = 50000;
    std::size_t line_bytes = 96;
    double duration_sec = 0.0;  // > 0 时改为持续压测，忽略 lines_per_conn
};

bool connect_to(const Options& opt, socket_t& out) {
    socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == kInvalidSocket) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(opt.port));
    if (::inet_pton(AF_INET, opt.host.c_str(), &addr.sin_addr) != 1) {
        sock_close(fd);
        return false;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        sock_close(fd);
        return false;
    }
    out = fd;
    return true;
}

std::string make_line(std::size_t bytes, int index) {
    std::string line = "{\"seq\":" + std::to_string(index) + ",\"msg\":\"benchmark payload\"}";
    while (line.size() < bytes) line.push_back('x');
    return line;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--host") opt.host = next("--host");
        else if (arg == "--port") opt.port = std::atoi(next("--port").c_str());
        else if (arg == "--conns") opt.connections = std::atoi(next("--conns").c_str());
        else if (arg == "--lines") opt.lines_per_conn = std::atoi(next("--lines").c_str());
        else if (arg == "--line-bytes") opt.line_bytes = static_cast<std::size_t>(std::atoi(next("--line-bytes").c_str()));
        else if (arg == "--duration") opt.duration_sec = std::atof(next("--duration").c_str());
        else {
            std::fprintf(stderr, "unknown arg %s\n", arg.c_str());
            return 2;
        }
    }
    if (opt.connections <= 0 || opt.lines_per_conn <= 0) return 2;
    net_init();

    std::atomic<std::uint64_t> sent_bytes{0};
    std::atomic<std::uint64_t> sent_lines{0};
    std::atomic<int> failed_conns{0};
    const auto start = std::chrono::steady_clock::now();
    const bool sustained = opt.duration_sec > 0.0;

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(opt.connections));
    for (int c = 0; c < opt.connections; ++c) {
        threads.emplace_back([&, c] {
            socket_t fd = kInvalidSocket;
            if (!connect_to(opt, fd)) {
                ++failed_conns;
                return;
            }
            std::string batch;
            batch.reserve(64 * 1024);
            std::uint64_t bytes = 0;
            std::uint64_t lines = 0;
            const auto deadline = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                              std::chrono::duration<double>(opt.duration_sec));
            const int limit = sustained ? 0x7fffffff : opt.lines_per_conn;
            for (int i = 0; i < limit; ++i) {
                if (sustained && std::chrono::steady_clock::now() >= deadline) break;
                batch += make_line(opt.line_bytes, i);
                batch.push_back('\n');
                ++lines;
                if (batch.size() >= 32 * 1024) {
                    const int n = sock_write(fd, batch.data(), static_cast<int>(batch.size()));
                    if (n <= 0) break;
                    bytes += static_cast<std::uint64_t>(n);
                    batch.clear();
                }
            }
            if (!batch.empty()) {
                const int n = sock_write(fd, batch.data(), static_cast<int>(batch.size()));
                if (n > 0) bytes += static_cast<std::uint64_t>(n);
            }
            sent_bytes += bytes;
            sent_lines += lines;
            sock_close(fd);
            (void)c;
        });
    }
    for (auto& t : threads) t.join();

    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const std::uint64_t total_lines = sent_lines.load();
    const double mib = static_cast<double>(sent_bytes.load()) / (1024.0 * 1024.0);

    std::printf("connections      : %d (failed: %d)\n", opt.connections, failed_conns.load());
    std::printf("lines sent       : %llu (x %zu bytes)\n",
                static_cast<unsigned long long>(total_lines), opt.line_bytes);
    std::printf("bytes sent       : %.1f MiB\n", mib);
    std::printf("elapsed          : %.3f s\n", elapsed);
    std::printf("throughput       : %.0f lines/s, %.1f MiB/s\n", total_lines / elapsed, mib / elapsed);
    net_cleanup();
    return failed_conns.load() == 0 ? 0 : 1;
}
