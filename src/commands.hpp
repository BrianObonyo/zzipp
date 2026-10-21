#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace zzipp {

struct Cancelled : std::runtime_error {
    Cancelled() : std::runtime_error("cancelled") {}
};

// Optional hooks that let a front end (the GUI) observe and control an operation.
// With all hooks unset (the CLI), messages go to stdout/stderr exactly as before.
struct Callbacks {
    std::function<void(uint64_t done, uint64_t total)> progress;  // total == 0: unknown
    std::function<void(const std::string&)> log;                  // replaces stdout/stderr output
    const std::atomic<bool>* cancel = nullptr;                    // set to true to request a stop

    bool cancelled() const { return cancel && cancel->load(); }
    void check() const {
        if (cancelled()) throw Cancelled();
    }
    void report(uint64_t done, uint64_t total) const {
        if (progress) progress(done, total);
    }
    void say(const std::string& m) const {
        if (log) log(m);
        else std::printf("%s\n", m.c_str());
    }
    void warn(const std::string& m) const {
        if (log) log("warning: " + m);
        else std::fprintf(stderr, "zzipp: warning: %s\n", m.c_str());
    }
};

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
    Callbacks cb;
};

struct ExtractOptions {
    std::string archive;
    std::string out_dir = ".";
    std::vector<std::string> paths;  // empty = everything
    unsigned threads = 1;
    bool force = false;
    bool verbose = false;
    Callbacks cb;
};

struct ZipOptions {
    std::string archive;
    std::string zip_path;
    int level = 6;  // zlib level 1-9
    bool force = false;
    Callbacks cb;
};

int cmd_create(const CreateOptions& opts);
int cmd_extract(const ExtractOptions& opts);
int cmd_list(const std::string& archive, bool long_format);
int cmd_test(const std::string& archive, unsigned threads, const Callbacks& cb = Callbacks());
int cmd_export_zip(const ZipOptions& opts);

}  // namespace zzipp
