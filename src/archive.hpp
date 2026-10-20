// Read-side access to a .mzg archive. All structure is validated on open, so the
// rest of the code can treat the Index as trustworthy (bounds, paths, offsets).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "format.hpp"

namespace zzipp {

// The index region at the end of the file is a small wrapper around the serialised
// index: [u8 method][u64 raw_size][u32 dict_size][payload]. Large archives have
// megabytes of paths and metadata, so the payload is LZMA2-compressed when it helps.
std::vector<uint8_t> encode_index_region(const Index& idx);
Index decode_index_region(const uint8_t* data, size_t size);  // throws FormatError

class ArchiveReader {
public:
    explicit ArchiveReader(const std::string& path);

    const Index& index() const { return index_; }
    uint64_t file_size() const { return file_size_; }

    // Reads block `b`, verifies its checksum and returns the decoded bytes.
    // Safe to call from several threads at once.
    std::vector<uint8_t> read_block(uint32_t b) const;

private:
    std::string path_;
    uint64_t file_size_ = 0;
    Index index_;
};

}  // namespace zzipp
