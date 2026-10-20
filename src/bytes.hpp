// Little-endian byte (de)serialisation with bounds checking.
// ByteReader never reads past its buffer: malformed input throws FormatError.
#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace zzipp {

struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class ByteWriter {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { le(v, 2); }
    void u32(uint32_t v) { le(v, 4); }
    void u64(uint64_t v) { le(v, 8); }
    void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }
    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        buf_.insert(buf_.end(), s.begin(), s.end());
    }
    void raw(const std::string& s) { buf_.insert(buf_.end(), s.begin(), s.end()); }
    const std::vector<uint8_t>& bytes() const { return buf_; }

private:
    void le(uint64_t v, int n) {
        for (int i = 0; i < n; ++i) buf_.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
    std::vector<uint8_t> buf_;
};

class ByteReader {
public:
    ByteReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    uint8_t u8() { need(1); return p_[pos_++]; }
    uint16_t u16() { return static_cast<uint16_t>(le(2)); }
    uint32_t u32() { return static_cast<uint32_t>(le(4)); }
    uint64_t u64() { return le(8); }
    int64_t i64() { return static_cast<int64_t>(le(8)); }
    std::string str() {
        uint32_t len = u32();
        need(len);
        std::string s(reinterpret_cast<const char*>(p_ + pos_), len);
        pos_ += len;
        return s;
    }
    size_t remaining() const { return n_ - pos_; }

private:
    void need(size_t k) const {
        if (k > n_ - pos_) throw FormatError("truncated data");
    }
    uint64_t le(int n) {
        need(static_cast<size_t>(n));
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= static_cast<uint64_t>(p_[pos_ + i]) << (8 * i);
        pos_ += static_cast<size_t>(n);
        return v;
    }
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
};

}  // namespace zzipp
