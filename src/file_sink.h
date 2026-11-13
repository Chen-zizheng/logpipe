#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

/**
 * 线程安全的落盘器：按大小轮转（app.log -> app.log.1 -> app.log.2 ...），
 * 超过 keep_files 的旧文件直接删掉。
 */
class FileSink {
public:
    FileSink(std::string dir, std::string base_name, std::uint64_t rotate_bytes, int keep_files);

    bool open();
    bool write(const char* data, std::size_t len);
    bool write(const std::string& data) { return write(data.data(), data.size()); }
    void flush();
    void close();

    std::uint64_t bytes_written() const;
    std::uint64_t rotations() const;
    std::string current_path() const;
    std::uint64_t current_size() const;

private:
    bool rotate_locked();

    mutable std::mutex mu_;
    std::string dir_;
    std::string base_name_;
    std::uint64_t rotate_bytes_;
    int keep_files_;

    std::ofstream out_;
    std::uint64_t current_size_ = 0;
    std::uint64_t total_written_ = 0;
    std::uint64_t rotations_ = 0;
    bool open_ = false;
};
