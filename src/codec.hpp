// Thin wrappers around liblzma's raw LZMA2 encoder/decoder (no .xz container overhead).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "format.hpp"

namespace zzipp {

// Compresses `n` bytes. Returns false if the result would not be smaller than the
// input (caller should store the block instead). `preset` is an xz preset (0-9,
// optionally OR-ed with LZMA_PRESET_EXTREME); `dict_request` of 0 uses the preset's
// dictionary size. The dictionary actually used is returned in `dict_used`.
bool lzma2_encode(const uint8_t* in, size_t n, bool bcj_x86, uint32_t preset, uint32_t dict_request,
                  std::vector<uint8_t>& out, uint32_t& dict_used);

// Decodes exactly `out_size` bytes. Throws FormatError if the stream is corrupt,
// truncated, or does not decode to exactly `out_size` bytes.
void lzma2_decode(const uint8_t* in, size_t n, Method method, uint32_t dict_size, uint8_t* out,
                  size_t out_size);

}  // namespace zzipp
