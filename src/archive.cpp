#include "archive.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <lzma.h>
#include <stdexcept>

#include "codec.hpp"
#include "util.hpp"

namespace zzipp {

namespace {
constexpr uint64_t MAX_INDEX_RAW = 1ull << 30;
}

std::vector<uint8_t> encode_index_region(const Index& idx) {
    const std::vector<uint8_t> raw = serialize_index(idx);
    std::vector<uint8_t> packed;
    uint32_t dict = 0;
    const bool ok = lzma2_encode(raw.data(), raw.size(), false, 9 | LZMA_PRESET_EXTREME, 0, packed, dict);
    ByteWriter w;
    w.u8(ok ? static_cast<uint8_t>(Method::Lzma2) : static_cast<uint8_t>(Method::Store));
    w.u64(raw.size());
    w.u32(ok ? dict : 0);
    std::vector<uint8_t> out = w.bytes();
    const std::vector<uint8_t>& body = ok ? packed : raw;
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

Index decode_index_region(const uint8_t* data, size_t size) {
    ByteReader r(data, size);
    const uint8_t m = r.u8();
    const uint64_t raw_size = r.u64();
    const uint32_t dict = r.u32();
    if (m > static_cast<uint8_t>(Method::Lzma2)) throw FormatError("unknown index encoding");
    if (raw_size == 0 || raw_size > MAX_INDEX_RAW) throw FormatError("implausible index size");
    const size_t header = 1 + 8 + 4;
    const uint8_t* payload = data + header;
    const size_t payload_size = size - header;
    std::vector<uint8_t> raw(static_cast<size_t>(raw_size));
    if (m == static_cast<uint8_t>(Method::Store)) {
        if (payload_size != raw.size()) throw FormatError("index size mismatch");
        std::copy(payload, payload + payload_size, raw.begin());
    } else {
        if (dict < MIN_DICT || dict > MAX_DICT) throw FormatError("invalid index dictionary size");
        lzma2_decode(payload, payload_size, Method::Lzma2, dict, raw.data(), raw.size());
    }
    return parse_index(raw.data(), raw.size());
}

ArchiveReader::ArchiveReader(const std::string& path) : path_(path) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("cannot open archive: " + path);
    file_size_ = static_cast<uint64_t>(in.tellg());
    if (file_size_ < HEADER_SIZE) throw FormatError("file too small to be an archive");

    std::vector<uint8_t> hdr(HEADER_SIZE);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(hdr.data()), HEADER_SIZE);
    if (!in) throw FormatError("cannot read archive header");
    const Header h = parse_header(hdr.data(), hdr.size());

    if (h.index_offset < HEADER_SIZE || h.index_offset > file_size_ ||
        h.index_size != file_size_ - h.index_offset)
        throw FormatError("archive is truncated or its index is damaged");

    std::vector<uint8_t> raw(h.index_size);
    in.seekg(static_cast<std::streamoff>(h.index_offset));
    in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    if (!in) throw FormatError("cannot read archive index");
    if (crc32(raw.data(), raw.size()) != h.index_crc) throw FormatError("index checksum mismatch");

    index_ = decode_index_region(raw.data(), raw.size());

    for (const auto& b : index_.blocks) {
        if (b.offset < HEADER_SIZE || b.offset > h.index_offset ||
            b.comp_size > h.index_offset - b.offset)
            throw FormatError("block lies outside the data area");
    }
}

std::vector<uint8_t> ArchiveReader::read_block(uint32_t b) const {
    if (b >= index_.blocks.size()) throw FormatError("block index out of range");
    const BlockInfo& bi = index_.blocks[b];

    std::ifstream in(std::filesystem::u8path(path_), std::ios::binary);
    if (!in) throw std::runtime_error("cannot open archive: " + path_);
    std::vector<uint8_t> comp(bi.comp_size);
    in.seekg(static_cast<std::streamoff>(bi.offset));
    in.read(reinterpret_cast<char*>(comp.data()), static_cast<std::streamsize>(comp.size()));
    if (!in) throw FormatError("block " + std::to_string(b) + " is truncated");
    if (crc32(comp.data(), comp.size()) != bi.comp_crc)
        throw FormatError("block " + std::to_string(b) + " is corrupt (checksum mismatch)");
    if (bi.method == Method::Store) return comp;

    std::vector<uint8_t> raw(bi.raw_size);
    lzma2_decode(comp.data(), comp.size(), bi.method, bi.dict_size, raw.data(), raw.size());
    return raw;
}

}  // namespace zzipp
