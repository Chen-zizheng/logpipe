#pragma once

#include <atomic>
#include <cstdint>
#include <string>

/** 运行期计数全部用原子变量，读的时候不需要加锁。 */
struct Metrics {
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> closed{0};
    std::atomic<std::uint64_t> bytes_in{0};
    std::atomic<std::uint64_t> records{0};
    std::atomic<std::uint64_t> written_bytes{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> rotations{0};
    std::atomic<std::uint64_t> http_requests{0};

    void reset();
    std::string to_json() const;
};
