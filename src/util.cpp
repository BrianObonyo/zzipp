#include "util.hpp"

#include <cctype>
#include <cmath>
#include <cstdarg>
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

std::string format_string(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = std::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    std::string s(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    if (n > 0) std::vsnprintf(&s[0], s.size() + 1, fmt, ap2);
    va_end(ap2);
    return s;
}

unsigned default_threads() {
    unsigned n = std::thread::hardware_concurrency();
    return std::max(1u, std::min(8u, n));
}

// std::filesystem's clock epoch is implementation-defined (Unix epoch on recent libstdc++, 1601 on
// Windows). The epochs differ by a whole number of seconds, so we measure that offset once, in
// seconds, and apply it in both directions. Doing the arithmetic in seconds (not nanoseconds)
// avoids integer overflow for far-away epochs.
namespace {
const int64_t g_clock_delta_sec = [] {
    auto f = fs::file_time_type::clock::now();
    auto s = std::chrono::system_clock::now();
    const double d = std::chrono::duration<double>(s.time_since_epoch()).count() -
                     std::chrono::duration<double>(f.time_since_epoch()).count();
    return static_cast<int64_t>(std::llround(d));  // sub-second part is only measurement noise
}();
// Range of Unix times this platform's file clock can represent (with a small safety margin).
const int64_t g_min_unix = std::chrono::duration_cast<std::chrono::seconds>(fs::file_time_type::min().time_since_epoch()).count() + g_clock_delta_sec + 2;
const int64_t g_max_unix = std::chrono::duration_cast<std::chrono::seconds>(fs::file_time_type::max().time_since_epoch()).count() + g_clock_delta_sec - 2;
}  // namespace

int64_t to_unix_time(fs::file_time_type t) {
    return std::chrono::floor<std::chrono::seconds>(t.time_since_epoch()).count() + g_clock_delta_sec;
}

fs::file_time_type from_unix_time(int64_t seconds) {
    // An archive can contain any 64-bit value; clamp instead of overflowing.
    seconds = std::max(g_min_unix, std::min(g_max_unix, seconds));
    return fs::file_time_type(std::chrono::duration_cast<fs::file_time_type::duration>(
        std::chrono::seconds(seconds - g_clock_delta_sec)));
}

}  // namespace zzipp
