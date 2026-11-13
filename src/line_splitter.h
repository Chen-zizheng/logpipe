#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

/**
 * TCP 是字节流：一次 recv 可能拿到半行，也可能一次拿到十几行。
 * LineSplitter 负责把字节流重新切成完整的行（去掉 \r\n），
 * 并且对"迟迟不出现换行的超长行"做截断，避免内存被一条坏数据拖爆。
 */
class LineSplitter {
public:
    using LineHandler = std::function<void(std::string_view)>;

    explicit LineSplitter(std::size_t max_line_bytes = 64 * 1024);

    void feed(const char* data, std::size_t len, const LineHandler& handler);

    /** 因为没有换行而被丢弃的超长行数量 */
    std::size_t oversize_dropped() const { return oversize_dropped_; }
    std::size_t bytes_seen() const { return bytes_seen_; }
    std::size_t lines_seen() const { return lines_seen_; }
    std::size_t pending_bytes() const { return buffer_.size(); }

    void reset();

private:
    std::string buffer_;
    std::size_t max_line_bytes_;
    std::size_t oversize_dropped_ = 0;
    std::size_t bytes_seen_ = 0;
    std::size_t lines_seen_ = 0;
    bool discarding_ = false;
};
