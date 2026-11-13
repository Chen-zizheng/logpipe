#include "line_splitter.h"

LineSplitter::LineSplitter(std::size_t max_line_bytes)
    : max_line_bytes_(max_line_bytes == 0 ? 1024 : max_line_bytes) {}

void LineSplitter::reset() {
    buffer_.clear();
    discarding_ = false;
}

void LineSplitter::feed(const char* data, std::size_t len, const LineHandler& handler) {
    if (data == nullptr || len == 0) return;
    bytes_seen_ += len;
    buffer_.append(data, len);

    std::size_t start = 0;
    for (;;) {
        std::size_t nl = buffer_.find('\n', start);
        if (nl == std::string::npos) break;

        std::size_t line_len = nl - start;
        if (discarding_) {
            // 超长行的尾部，丢弃到换行为止
            discarding_ = false;
        } else {
            std::size_t end = start + line_len;
            if (end > start && buffer_[end - 1] == '\r') --end;
            ++lines_seen_;
            handler(std::string_view(buffer_.data() + start, end - start));
        }
        start = nl + 1;
    }

    if (start > 0) buffer_.erase(0, start);

    if (buffer_.size() > max_line_bytes_) {
        // 一直没有换行：整段丢掉，进入"丢弃到下一个换行"的状态
        buffer_.clear();
        discarding_ = true;
        ++oversize_dropped_;
    }
}
