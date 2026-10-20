// On-disk format of a .mzg archive. See docs/FORMAT.md for the full spec.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "bytes.hpp"

namespace zzipp {

inline constexpr char MAGIC[4] = {'M', 'Z', 'G', '1'};
inline constexpr uint16_t FORMAT_VERSION = 1;
inline constexpr size_t HEADER_SIZE = 32;
inline constexpr uint32_t NO_BLOCK = 0xFFFFFFFFu;
inline constexpr uint64_t MAX_BLOCK_SIZE = 4ull << 30;  // sanity cap on one block (raw bytes)
inline constexpr uint32_t MIN_DICT = 4096;
inline constexpr uint32_t MAX_DICT = 1536u << 20;

enum class Method : uint8_t {
    Store = 0,       // raw bytes
    Lzma2 = 1,       // raw LZMA2 stream
    Lzma2Bcj86 = 2,  // x86 BCJ filter, then raw LZMA2 stream
};

enum class EntryKind : uint8_t { File = 0, Dir = 1 };

struct BlockInfo {
    uint64_t offset = 0;     // absolute file offset of the compressed bytes
    uint64_t comp_size = 0;  // bytes on disk
    uint64_t raw_size = 0;   // bytes after decoding
    uint32_t comp_crc = 0;   // CRC32 of the compressed bytes
    uint32_t dict_size = 0;  // LZMA2 dictionary size (0 for Store)
    Method method = Method::Store;
};

struct Entry {
    EntryKind kind = EntryKind::File;
    std::string path;  // relative, '/' separated, never starts with '/' or contains ".."
    uint64_t size = 0;
    int64_t mtime = 0;  // seconds since the Unix epoch
    uint32_t mode = 0;  // permission bits (0777 mask)
    uint32_t crc = 0;   // CRC32 of the file contents
    uint32_t block = NO_BLOCK;
    uint64_t offset = 0;  // byte offset of this file inside the decoded block
};

struct Header {
    uint64_t index_offset = 0;
    uint64_t index_size = 0;
    uint32_t index_crc = 0;
};

struct Index {
    std::vector<BlockInfo> blocks;
    std::vector<Entry> entries;
};

bool is_safe_relative_path(const std::string& path);

std::array<uint8_t, HEADER_SIZE> serialize_header(const Header& h);
Header parse_header(const uint8_t* data, size_t size);

std::vector<uint8_t> serialize_index(const Index& idx);
Index parse_index(const uint8_t* data, size_t size);  // throws FormatError on any inconsistency

}  // namespace zzipp
