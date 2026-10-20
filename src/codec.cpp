#include "codec.hpp"

#include <lzma.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace zzipp {

namespace {
void make_chain(lzma_filter* f, lzma_options_lzma& opts, bool bcj) {
    int i = 0;
    if (bcj) {
        f[i].id = LZMA_FILTER_X86;
        f[i].options = nullptr;
        ++i;
    }
    f[i].id = LZMA_FILTER_LZMA2;
    f[i].options = &opts;
    ++i;
    f[i].id = LZMA_VLI_UNKNOWN;
    f[i].options = nullptr;
}
}  // namespace

bool lzma2_encode(const uint8_t* in, size_t n, bool bcj_x86, uint32_t preset, uint32_t dict_request,
                  std::vector<uint8_t>& out, uint32_t& dict_used) {
    if (n == 0) return false;
    lzma_options_lzma opts;
    if (lzma_lzma_preset(&opts, preset)) throw std::runtime_error("invalid compression preset");
    uint64_t dict = dict_request ? dict_request : opts.dict_size;
    // A dictionary larger than the block can never be used: shrink it to save memory.
    dict = std::min<uint64_t>(dict, n);
    dict = std::max<uint64_t>(dict, MIN_DICT);
    dict = std::min<uint64_t>(dict, MAX_DICT);
    opts.dict_size = static_cast<uint32_t>(dict);

    lzma_filter chain[LZMA_FILTERS_MAX + 1];
    make_chain(chain, opts, bcj_x86);

    // Only accept output strictly smaller than the input; otherwise liblzma reports
    // LZMA_BUF_ERROR and we fall back to storing the block.
    out.resize(n - 1);
    size_t out_pos = 0;
    lzma_ret r = lzma_raw_buffer_encode(chain, nullptr, in, n, out.data(), &out_pos, out.size());
    if (r == LZMA_OK) {
        out.resize(out_pos);
        dict_used = static_cast<uint32_t>(dict);
        return true;
    }
    if (r == LZMA_BUF_ERROR) {
        out.clear();
        return false;
    }
    throw std::runtime_error("LZMA encoder failed (code " + std::to_string(static_cast<int>(r)) + ")");
}

void lzma2_decode(const uint8_t* in, size_t n, Method method, uint32_t dict_size, uint8_t* out,
                  size_t out_size) {
    if (method == Method::Store) {
        if (n != out_size) throw FormatError("stored block size mismatch");
        std::memcpy(out, in, n);
        return;
    }
    lzma_options_lzma opts;
    if (lzma_lzma_preset(&opts, 6)) throw std::runtime_error("liblzma preset failure");
    opts.dict_size = dict_size;
    lzma_filter chain[LZMA_FILTERS_MAX + 1];
    make_chain(chain, opts, method == Method::Lzma2Bcj86);

    size_t in_pos = 0, out_pos = 0;
    lzma_ret r = lzma_raw_buffer_decode(chain, nullptr, in, &in_pos, n, out, &out_pos, out_size);
    if (r == LZMA_MEM_ERROR) throw std::runtime_error("out of memory while decoding");
    if (r != LZMA_OK || in_pos != n || out_pos != out_size)
        throw FormatError("corrupt compressed data");
}

}  // namespace zzipp
