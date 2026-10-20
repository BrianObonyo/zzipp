#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace zzipp {

struct CreateOptions {
    std::string archive;
    std::vector<std::string> inputs;
    uint32_t preset = 6;   // xz preset 0-9 (OR-ed with LZMA_PRESET_EXTREME for --max)
    uint32_t dict = 0;     // bytes; 0 = preset default
    uint64_t block = 32ull << 20;
    unsigned threads = 1;
    bool dedup = true;
    bool sort = true;
    bool force = false;
    bool verbose = false;
};

struct ExtractOptions {
    std::string archive;
    std::string out_dir = ".";
    std::vector<std::string> paths;  // empty = everything
    unsigned threads = 1;
    bool force = false;
    bool verbose = false;
};

struct ZipOptions {
    std::string archive;
    std::string zip_path;
    int level = 6;  // zlib level 1-9
    bool force = false;
};

int cmd_create(const CreateOptions& opts);
int cmd_extract(const ExtractOptions& opts);
int cmd_list(const std::string& archive, bool long_format);
int cmd_test(const std::string& archive, unsigned threads);
int cmd_export_zip(const ZipOptions& opts);

}  // namespace zzipp
