// logpipe：TCP 日志接收 -> 有界队列 -> 线程池写盘（按大小轮转），
// 同一个端口还提供 /api/stats 与 /api/tail 给网页看板用。
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "platform.h"
#include "server.h"

namespace {

std::atomic<bool> g_stop{false};

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop = true;
        return TRUE;
    }
    return FALSE;
}
#else
void on_signal(int) { g_stop = true; }
#endif

void print_usage() {
    std::printf(
        "logpipe - TCP log collector with size-based rotation\n"
        "\n"
        "usage: logpipe [options]\n"
        "  --host <addr>        listen address (default 0.0.0.0)\n"
        "  --port <n>           listen port (default 9000, 0 = auto)\n"
        "  --out <dir>          log directory (default ./logs)\n"
        "  --name <file>        active file name (default app.log)\n"
        "  --rotate-mb <n>      rotate when file exceeds n MiB (default 16)\n"
        "  --keep <n>           keep n rotated files (default 5)\n"
        "  --threads <n>        writer threads (default 2)\n"
        "  --queue <n>          queue capacity, overflow drops (default 8192)\n"
        "  --max-line <bytes>   longest accepted line (default 65536)\n"
        "  --web <dir>          dashboard assets (default ./web)\n"
        "  --help               show this help\n");
}

std::string next_arg(int argc, char** argv, int& i, const char* name) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "logpipe: missing value for %s\n", name);
        std::exit(2);
    }
    return argv[++i];
}

}  // namespace

int main(int argc, char** argv) {
    ServerConfig cfg;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else if (arg == "--host") {
            cfg.host = next_arg(argc, argv, i, "--host");
        } else if (arg == "--port") {
            cfg.port = parse_port(next_arg(argc, argv, i, "--port"));
            if (cfg.port < 0) {
                std::fprintf(stderr, "logpipe: invalid port\n");
                return 2;
            }
        } else if (arg == "--out") {
            cfg.out_dir = next_arg(argc, argv, i, "--out");
        } else if (arg == "--name") {
            cfg.base_name = next_arg(argc, argv, i, "--name");
        } else if (arg == "--rotate-mb") {
            cfg.rotate_bytes = static_cast<std::uint64_t>(std::atoll(next_arg(argc, argv, i, "--rotate-mb").c_str())) * 1024ull * 1024ull;
        } else if (arg == "--keep") {
            cfg.keep_files = std::atoi(next_arg(argc, argv, i, "--keep").c_str());
        } else if (arg == "--threads") {
            cfg.writer_threads = std::atoi(next_arg(argc, argv, i, "--threads").c_str());
        } else if (arg == "--queue") {
            cfg.queue_capacity = static_cast<std::size_t>(std::atoll(next_arg(argc, argv, i, "--queue").c_str()));
        } else if (arg == "--max-line") {
            cfg.max_line_bytes = static_cast<std::size_t>(std::atoll(next_arg(argc, argv, i, "--max-line").c_str()));
        } else if (arg == "--web") {
            cfg.web_dir = next_arg(argc, argv, i, "--web");
        } else {
            std::fprintf(stderr, "logpipe: unknown option %s (try --help)\n", arg.c_str());
            return 2;
        }
    }

#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);
#endif

    Server server(cfg);
    if (!server.start()) return 1;

    std::printf("logpipe: dashboard http://127.0.0.1:%d/\n", server.port());
    std::fflush(stdout);

    std::thread watchdog([&server] {
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::printf("logpipe: shutting down\n");
        server.stop();
    });

    server.run();

    g_stop = true;
    if (watchdog.joinable()) watchdog.join();
    net_cleanup();
    return 0;
}
