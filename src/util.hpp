#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <lzma.h>

namespace zzipp {

inline constexpr uint64_t KiB = 1024ull, MiB = KiB * 1024, GiB = MiB * 1024;

inline uint32_t crc32(const uint8_t* d, size_t n, uint32_t prev = 0) { return lzma_crc32(d, n, prev); }
inline uint64_t crc64(const uint8_t* d, size_t n, uint64_t prev = 0) { return lzma_crc64(d, n, prev); }

uint64_t parse_size(const std::string& text);  // "64m", "512k", "1g", "4096"
std::string human_size(uint64_t bytes);        // "12.3 MiB"
std::string group_digits(uint64_t n);          // 1234567 -> "1,234,567"
std::string format_string(const char* fmt, ...);  // printf into a std::string
unsigned default_threads();

int64_t to_unix_time(std::filesystem::file_time_type t);
std::filesystem::file_time_type from_unix_time(int64_t seconds);

class Timer {
public:
    Timer() : start_(std::chrono::steady_clock::now()) {}
    double seconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }

private:
    std::chrono::steady_clock::time_point start_;
};

// Runs fn(i) for i in [0, count) on up to `threads` threads. The first exception
// thrown by any call stops the remaining work and is rethrown to the caller.
template <class Fn>
void parallel_for(size_t count, unsigned threads, Fn&& fn) {
    if (count == 0) return;
    threads = static_cast<unsigned>(std::max<size_t>(1, std::min<size_t>(threads, count)));
    std::atomic<size_t> next{0};
    std::atomic<bool> abort{false};
    std::exception_ptr first;
    std::mutex mu;
    auto worker = [&] {
        for (;;) {
            if (abort.load()) return;
            size_t i = next.fetch_add(1);
            if (i >= count) return;
            try {
                fn(i);
            } catch (...) {
                std::lock_guard<std::mutex> lk(mu);
                if (!first) first = std::current_exception();
                abort = true;
                return;
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker);
    worker();
    for (auto& th : pool) th.join();
    if (first) std::rethrow_exception(first);
}

}  // namespace zzipp
