#include "format.hpp"

#include <cstring>
#include <unordered_set>

namespace zzipp {

namespace {
constexpr size_t BLOCK_RECORD_MIN = 8 + 8 + 8 + 4 + 4 + 1;
constexpr size_t ENTRY_RECORD_MIN = 1 + 4 + 8 + 8 + 4 + 4 + 4 + 8;
}  // namespace

bool is_safe_relative_path(const std::string& p) {
    if (p.empty() || p.front() == '/') return false;
    size_t start = 0;
    for (;;) {
        size_t end = p.find('/', start);
        if (end == std::string::npos) end = p.size();
        const std::string comp = p.substr(start, end - start);
        if (comp.empty() || comp == "." || comp == "..") return false;
        for (char c : comp) {
            if (c == '\0' || c == '\\') return false;
#ifdef _WIN32
            if (c == ':') return false;
#endif
        }
        if (end == p.size()) break;
        start = end + 1;
    }
    return true;
}

std::array<uint8_t, HEADER_SIZE> serialize_header(const Header& h) {
    ByteWriter w;
    w.raw(std::string(MAGIC, 4));
    w.u16(FORMAT_VERSION);
    w.u16(0);  // flags (reserved)
    w.u64(h.index_offset);
    w.u64(h.index_size);
    w.u32(h.index_crc);
    w.u32(0);  // reserved
    std::array<uint8_t, HEADER_SIZE> out{};
    std::memcpy(out.data(), w.bytes().data(), HEADER_SIZE);
    return out;
}

Header parse_header(const uint8_t* data, size_t size) {
    if (size < HEADER_SIZE) throw FormatError("file too small to be an archive");
    ByteReader r(data, size);
    uint8_t magic[4];
    for (auto& b : magic) b = r.u8();
    if (std::memcmp(magic, MAGIC, 4) != 0) throw FormatError("not a zzipp archive (bad magic)");
    uint16_t version = r.u16();
    if (version != FORMAT_VERSION)
        throw FormatError("unsupported archive version " + std::to_string(version));
    r.u16();  // flags
    Header h;
    h.index_offset = r.u64();
    h.index_size = r.u64();
    h.index_crc = r.u32();
    return h;
}

std::vector<uint8_t> serialize_index(const Index& idx) {
    ByteWriter w;
    w.u32(static_cast<uint32_t>(idx.blocks.size()));
    for (const auto& b : idx.blocks) {
        w.u64(b.offset);
        w.u64(b.comp_size);
        w.u64(b.raw_size);
        w.u32(b.comp_crc);
        w.u32(b.dict_size);
        w.u8(static_cast<uint8_t>(b.method));
    }
    w.u32(static_cast<uint32_t>(idx.entries.size()));
    for (const auto& e : idx.entries) {
        w.u8(static_cast<uint8_t>(e.kind));
        w.str(e.path);
        w.u64(e.size);
        w.i64(e.mtime);
        w.u32(e.mode);
        w.u32(e.crc);
        w.u32(e.block);
        w.u64(e.offset);
    }
    return w.bytes();
}

Index parse_index(const uint8_t* data, size_t size) {
    ByteReader r(data, size);
    Index idx;

    uint32_t nblocks = r.u32();
    if (nblocks > r.remaining() / BLOCK_RECORD_MIN) throw FormatError("implausible block count");
    idx.blocks.reserve(nblocks);
    for (uint32_t i = 0; i < nblocks; ++i) {
        BlockInfo b;
        b.offset = r.u64();
        b.comp_size = r.u64();
        b.raw_size = r.u64();
        b.comp_crc = r.u32();
        b.dict_size = r.u32();
        uint8_t m = r.u8();
        if (m > static_cast<uint8_t>(Method::Lzma2Bcj86)) throw FormatError("unknown compression method");
        b.method = static_cast<Method>(m);
        if (b.raw_size == 0 || b.raw_size > MAX_BLOCK_SIZE || b.comp_size > MAX_BLOCK_SIZE)
            throw FormatError("implausible block size");
        if (b.method == Method::Store) {
            if (b.comp_size != b.raw_size) throw FormatError("stored block size mismatch");
        } else if (b.dict_size < MIN_DICT || b.dict_size > MAX_DICT) {
            throw FormatError("invalid dictionary size");
        }
        idx.blocks.push_back(b);
    }

    uint32_t nentries = r.u32();
    if (nentries > r.remaining() / ENTRY_RECORD_MIN) throw FormatError("implausible entry count");
    idx.entries.reserve(nentries);
    std::unordered_set<std::string> seen;
    seen.reserve(nentries);
    for (uint32_t i = 0; i < nentries; ++i) {
        Entry e;
        uint8_t k = r.u8();
        if (k > 1) throw FormatError("unknown entry kind");
        e.kind = static_cast<EntryKind>(k);
        e.path = r.str();
        e.size = r.u64();
        e.mtime = r.i64();
        e.mode = r.u32();
        e.crc = r.u32();
        e.block = r.u32();
        e.offset = r.u64();
        if (!is_safe_relative_path(e.path)) throw FormatError("unsafe path in archive: " + e.path);
        if (!seen.insert(e.path).second) throw FormatError("duplicate path in archive: " + e.path);
        if (e.kind == EntryKind::Dir) {
            if (e.size != 0 || e.block != NO_BLOCK) throw FormatError("malformed directory entry");
        } else if (e.block == NO_BLOCK) {
            if (e.size != 0) throw FormatError("file without data block has non-zero size");
        } else {
            if (e.block >= idx.blocks.size()) throw FormatError("entry references missing block");
            const uint64_t raw = idx.blocks[e.block].raw_size;
            if (e.size == 0 || e.offset > raw || e.size > raw - e.offset)
                throw FormatError("entry exceeds its block");
        }
        idx.entries.push_back(std::move(e));
    }
    if (r.remaining() != 0) throw FormatError("trailing bytes after index");
    return idx;
}

}  // namespace zzipp
