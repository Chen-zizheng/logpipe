// 极简测试：不引第三方框架，保证 clone 下来 cmake 就能跑。
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "file_sink.h"
#include "http.h"
#include "line_splitter.h"
#include "metrics.h"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

#define CHECK_EQ(actual, expected)                                                       \
    do {                                                                                 \
        ++g_checks;                                                                      \
        const auto a_ = (actual);                                                        \
        const auto e_ = (expected);                                                      \
        if (!(a_ == e_)) {                                                               \
            ++g_failures;                                                                \
            std::printf("FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #actual, #expected); \
        }                                                                                \
    } while (0)

std::vector<std::string> collect(LineSplitter& splitter, const std::string& chunk) {
    std::vector<std::string> lines;
    splitter.feed(chunk.data(), chunk.size(), [&lines](std::string_view line) {
        lines.emplace_back(line);
    });
    return lines;
}

void test_line_splitter_basic() {
    LineSplitter splitter;
    auto lines = collect(splitter, "alpha\nbeta\n");
    CHECK_EQ(lines.size(), std::size_t{2});
    CHECK(lines[0] == "alpha");
    CHECK(lines[1] == "beta");
    CHECK_EQ(splitter.lines_seen(), std::size_t{2});
}

void test_line_splitter_partial() {
    LineSplitter splitter;
    CHECK(collect(splitter, "hel").empty());
    CHECK_EQ(splitter.pending_bytes(), std::size_t{3});
    auto lines = collect(splitter, "lo\nwor");
    CHECK_EQ(lines.size(), std::size_t{1});
    CHECK(lines[0] == "hello");
    CHECK_EQ(splitter.pending_bytes(), std::size_t{3});
    lines = collect(splitter, "ld\n");
    CHECK_EQ(lines.size(), std::size_t{1});
    CHECK(lines[0] == "world");
    CHECK_EQ(splitter.pending_bytes(), std::size_t{0});
}

void test_line_splitter_crlf_and_empty() {
    LineSplitter splitter;
    auto lines = collect(splitter, "a\r\n\r\nb\r\n");
    CHECK_EQ(lines.size(), std::size_t{3});
    CHECK(lines[0] == "a");
    CHECK(lines[1].empty());
    CHECK(lines[2] == "b");
}

void test_line_splitter_oversize() {
    LineSplitter splitter(16);
    auto lines = collect(splitter, std::string(64, 'x'));
    CHECK(lines.empty());
    CHECK_EQ(splitter.oversize_dropped(), std::size_t{1});
    // 下一行应当正常恢复
    lines = collect(splitter, "\nok\n");
    CHECK_EQ(lines.size(), std::size_t{1});
    CHECK(lines[0] == "ok");
}

void test_http_parse() {
    HttpRequest req;
    const bool ok = http_parse_request("GET /api/tail?n=20 HTTP/1.1\r\nHost: x\r\n\r\n", req);
    CHECK(ok);
    CHECK(req.method == "GET");
    CHECK(req.path == "/api/tail");
    CHECK(req.query == "n=20");
    CHECK(http_request_complete("GET / HTTP/1.1\r\n\r\n"));
    CHECK(!http_request_complete("GET / HTTP/1.1\r\n"));
    CHECK(!http_parse_request("nonsense", req));
}

void test_http_query_int() {
    CHECK_EQ(http_query_int("n=20", "n", 5, 1, 100), 20);
    CHECK_EQ(http_query_int("n=5000", "n", 5, 1, 100), 100);
    CHECK_EQ(http_query_int("n=0", "n", 5, 1, 100), 1);
    CHECK_EQ(http_query_int("x=1", "n", 5, 1, 100), 5);
    CHECK_EQ(http_query_int("a=1&n=7", "n", 5, 1, 100), 7);
    CHECK_EQ(http_query_int("n=abc", "n", 5, 1, 100), 5);
}

void test_http_response() {
    const std::string res = http_response(200, "application/json", "{\"ok\":true}");
    CHECK(res.rfind("HTTP/1.1 200 OK", 0) == 0);
    CHECK(res.find("Content-Length: 11") != std::string::npos);
    CHECK(res.find("Connection: close") != std::string::npos);
    CHECK(http_content_type_for("index.html").find("text/html") == 0);
    CHECK(http_content_type_for("app.js").find("javascript") != std::string::npos);
}

void test_file_sink_rotation() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "logpipe_sink_test";
    fs::remove_all(dir);

    FileSink sink(dir.string(), "app.log", 40, 3);
    CHECK(sink.open());
    CHECK(sink.write(std::string(10, 'a')));
    CHECK(sink.write(std::string(10, 'b')));
    CHECK_EQ(sink.rotations(), std::uint64_t{0});
    CHECK_EQ(sink.current_size(), std::uint64_t{20});
    // 正好写到上限不轮转，超出才轮转
    CHECK(sink.write(std::string(20, 'c')));
    CHECK_EQ(sink.rotations(), std::uint64_t{0});
    CHECK(sink.write(std::string(25, 'd')));
    CHECK_EQ(sink.rotations(), std::uint64_t{1});
    CHECK_EQ(sink.bytes_written(), std::uint64_t{65});
    sink.flush();  // ofstream 有缓冲，量文件大小前必须刷盘
    CHECK(fs::exists(dir / "app.log.1"));
    CHECK_EQ(fs::file_size(dir / "app.log"), std::uint64_t{25});
    CHECK_EQ(fs::file_size(dir / "app.log.1"), std::uint64_t{40});
    sink.close();
    fs::remove_all(dir);
}

void test_metrics_json() {
    Metrics m;
    m.records = 3;
    m.dropped = 1;
    const std::string json = m.to_json();
    CHECK(json.find("\"records\":3") != std::string::npos);
    CHECK(json.find("\"dropped\":1") != std::string::npos);
    m.reset();
    CHECK_EQ(m.records.load(), std::uint64_t{0});
}

}  // namespace

int main() {
    test_line_splitter_basic();
    test_line_splitter_partial();
    test_line_splitter_crlf_and_empty();
    test_line_splitter_oversize();
    test_http_parse();
    test_http_query_int();
    test_http_response();
    test_file_sink_rotation();
    test_metrics_json();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
