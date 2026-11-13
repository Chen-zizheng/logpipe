#include "file_sink.h"

#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

FileSink::FileSink(std::string dir, std::string base_name, std::uint64_t rotate_bytes, int keep_files)
    : dir_(std::move(dir)),
      base_name_(std::move(base_name)),
      rotate_bytes_(rotate_bytes == 0 ? 16ull * 1024 * 1024 : rotate_bytes),
      keep_files_(keep_files < 0 ? 0 : keep_files) {}

std::string FileSink::current_path() const {
    std::lock_guard<std::mutex> lock(mu_);
    return (fs::path(dir_) / base_name_).string();
}

bool FileSink::open() {
    std::lock_guard<std::mutex> lock(mu_);
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) return false;

    const auto path = fs::path(dir_) / base_name_;
    out_.open(path, std::ios::binary | std::ios::app);
    if (!out_.is_open()) return false;

    std::error_code size_ec;
    auto size = fs::file_size(path, size_ec);
    current_size_ = size_ec ? 0 : static_cast<std::uint64_t>(size);
    open_ = true;
    return true;
}

bool FileSink::rotate_locked() {
    out_.flush();
    out_.close();
    open_ = false;

    const auto base = fs::path(dir_) / base_name_;
    std::error_code ec;

    if (keep_files_ > 0) {
        // 把最老的一份删掉，其余依次后移
        fs::path oldest = base.string() + "." + std::to_string(keep_files_);
        fs::remove(oldest, ec);
        for (int i = keep_files_ - 1; i >= 1; --i) {
            fs::path from = base.string() + "." + std::to_string(i);
            fs::path to = base.string() + "." + std::to_string(i + 1);
            if (fs::exists(from, ec)) fs::rename(from, to, ec);
        }
        fs::rename(base, base.string() + ".1", ec);
    } else {
        fs::remove(base, ec);
    }

    out_.open(base, std::ios::binary | std::ios::trunc);
    if (!out_.is_open()) return false;
    current_size_ = 0;
    open_ = true;
    ++rotations_;
    return true;
}

bool FileSink::write(const char* data, std::size_t len) {
    if (data == nullptr || len == 0) return true;
    std::lock_guard<std::mutex> lock(mu_);
    if (!open_) return false;

    if (current_size_ > 0 && current_size_ + len > rotate_bytes_) {
        if (!rotate_locked()) return false;
    }

    out_.write(data, static_cast<std::streamsize>(len));
    if (!out_) return false;

    current_size_ += len;
    total_written_ += len;
    return true;
}

void FileSink::flush() {
    std::lock_guard<std::mutex> lock(mu_);
    if (out_.is_open()) out_.flush();
}

void FileSink::close() {
    std::lock_guard<std::mutex> lock(mu_);
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
    open_ = false;
}

std::uint64_t FileSink::bytes_written() const {
    std::lock_guard<std::mutex> lock(mu_);
    return total_written_;
}

std::uint64_t FileSink::rotations() const {
    std::lock_guard<std::mutex> lock(mu_);
    return rotations_;
}

std::uint64_t FileSink::current_size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return current_size_;
}
