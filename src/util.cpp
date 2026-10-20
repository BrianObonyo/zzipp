#include "util.hpp"

#include <cctype>
#include <cstdio>
#include <stdexcept>

namespace fs = std::filesystem;

namespace zzipp {

uint64_t parse_size(const std::string& text) {
    size_t i = 0;
    uint64_t v = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        v = v * 10 + static_cast<uint64_t>(text[i] - '0');
        if (v > (1ull << 50)) throw std::invalid_argument("size too large: " + text);
        ++i;
    }
    if (i == 0) throw std::invalid_argument("invalid size: '" + text + "'");
    std::string suffix;
    for (size_t j = i; j < text.size(); ++j)
        suffix.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(text[j]))));
    uint64_t mul = 1;
    if (suffix.empty() || suffix == "b") mul = 1;
    else if (suffix == "k" || suffix == "kb" || suffix == "kib") mul = KiB;
    else if (suffix == "m" || suffix == "mb" || suffix == "mib") mul = MiB;
    else if (suffix == "g" || suffix == "gb" || suffix == "gib") mul = GiB;
    else throw std::invalid_argument("invalid size suffix: '" + text + "'");
    return v * mul;
}

std::string human_size(uint64_t bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    if (u == 0) std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    else std::snprintf(buf, sizeof buf, "%.1f %s", v, units[u]);
    return buf;
}

std::string group_digits(uint64_t n) {
    std::string s = std::to_string(n), out;
    int c = 0;
    for (size_t i = s.size(); i-- > 0;) {
        out.push_back(s[i]);
        if (++c % 3 == 0 && i != 0) out.push_back(',');
    }
    return std::string(out.rbegin(), out.rend());
}

unsigned default_threads() {
    unsigned n = std::thread::hardware_concurrency();
    return std::max(1u, std::min(8u, n));
}

// std::filesystem's clock epoch is implementation-defined before C++20, so we measure
// the offset to the system clock once and apply it consistently in both directions.
namespace {
const std::chrono::nanoseconds g_clock_delta = [] {
    auto f = fs::file_time_type::clock::now();
    auto s = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(s.time_since_epoch()) -
           std::chrono::duration_cast<std::chrono::nanoseconds>(f.time_since_epoch());
}();
}

int64_t to_unix_time(fs::file_time_type t) {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()) + g_clock_delta;
    int64_t n = ns.count();
    int64_t s = n / 1'000'000'000;
    if (n % 1'000'000'000 < 0) --s;
    return s;
}

fs::file_time_type from_unix_time(int64_t seconds) {
    auto d = std::chrono::seconds(seconds) - g_clock_delta;
    return fs::file_time_type(std::chrono::duration_cast<fs::file_time_type::duration>(d));
}

}  // namespace zzipp
