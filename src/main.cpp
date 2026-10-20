#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <lzma.h>

#include "commands.hpp"
#include "util.hpp"

#ifndef zzipp_VERSION
#define zzipp_VERSION "dev"
#endif

using namespace zzipp;

namespace {

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void print_usage(FILE* f) {
    std::fputs(
        "zzipp " zzipp_VERSION " - solid-compression archiver (LZMA2) with ZIP export\n"
        "\n"
        "Usage:\n"
        "  zzipp create  [options] <archive.mzg> <file|dir>...\n"
        "  zzipp extract [-o DIR] [-f] [-j N] <archive.mzg> [path...]\n"
        "  zzipp list    [-l] <archive.mzg>\n"
        "  zzipp test    [-j N] <archive.mzg>\n"
        "  zzipp export-zip [-f] [--level N] <archive.mzg> <out.zip>\n"
        "\n"
        "create options:\n"
        "  --fast | --normal | --max   speed/ratio preset (default: --normal)\n"
        "  --level N                   LZMA preset 0-9 (overrides the above)\n"
        "  --dict SIZE                 dictionary size, e.g. 64m (default: from preset)\n"
        "  --block SIZE                solid block size, e.g. 64m (default: 32m)\n"
        "  -j, --threads N             worker threads (default: CPU count, max 8)\n"
        "  --no-dedup                  keep identical files as separate data\n"
        "  --no-sort                   do not group files by extension\n"
        "  -f, --force                 overwrite an existing archive\n"
        "  -v, --verbose               print per-block progress\n",
        f);
}

std::string need_value(const std::vector<std::string>& a, size_t& i) {
    if (i + 1 >= a.size()) throw UsageError("option " + a[i] + " needs a value");
    return a[++i];
}

unsigned parse_threads(const std::string& v) {
    long n = std::strtol(v.c_str(), nullptr, 10);
    if (n < 1 || n > 256) throw UsageError("invalid thread count: " + v);
    return static_cast<unsigned>(n);
}

int run(const std::vector<std::string>& a) {
    if (a.empty()) {
        print_usage(stderr);
        return 2;
    }
    const std::string& cmd = a[0];
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        print_usage(stdout);
        return 0;
    }
    if (cmd == "version" || cmd == "--version") {
        std::printf("zzipp %s (liblzma %s)\n", zzipp_VERSION, lzma_version_string());
        return 0;
    }

    std::vector<std::string> pos;
    if (cmd == "create" || cmd == "c") {
        CreateOptions o;
        o.threads = default_threads();
        bool preset_set = false, max = false;
        for (size_t i = 1; i < a.size(); ++i) {
            const std::string& s = a[i];
            if (s == "--fast") { o.preset = 1; preset_set = true; }
            else if (s == "--normal") { o.preset = 6; preset_set = true; }
            else if (s == "--max") { o.preset = 9; max = true; preset_set = true; }
            else if (s == "--level") {
                long n = std::strtol(need_value(a, i).c_str(), nullptr, 10);
                if (n < 0 || n > 9) throw UsageError("--level must be 0-9");
                o.preset = static_cast<uint32_t>(n);
                preset_set = true;
            }
            else if (s == "--dict") o.dict = static_cast<uint32_t>(std::min<uint64_t>(parse_size(need_value(a, i)), 1536 * MiB));
            else if (s == "--block") o.block = std::max<uint64_t>(parse_size(need_value(a, i)), 1);
            else if (s == "-j" || s == "--threads") o.threads = parse_threads(need_value(a, i));
            else if (s == "--no-dedup") o.dedup = false;
            else if (s == "--no-sort") o.sort = false;
            else if (s == "-f" || s == "--force") o.force = true;
            else if (s == "-v" || s == "--verbose") o.verbose = true;
            else if (s.size() > 1 && s[0] == '-') throw UsageError("unknown option: " + s);
            else pos.push_back(s);
        }
        (void)preset_set;
        if (max) o.preset |= LZMA_PRESET_EXTREME;
        if (pos.size() < 2) throw UsageError("create needs an archive name and at least one input");
        o.archive = pos[0];
        o.inputs.assign(pos.begin() + 1, pos.end());
        return cmd_create(o);
    }
    if (cmd == "extract" || cmd == "x") {
        ExtractOptions o;
        o.threads = default_threads();
        for (size_t i = 1; i < a.size(); ++i) {
            const std::string& s = a[i];
            if (s == "-o" || s == "--output") o.out_dir = need_value(a, i);
            else if (s == "-f" || s == "--force") o.force = true;
            else if (s == "-j" || s == "--threads") o.threads = parse_threads(need_value(a, i));
            else if (s == "-v" || s == "--verbose") o.verbose = true;
            else if (s.size() > 1 && s[0] == '-') throw UsageError("unknown option: " + s);
            else pos.push_back(s);
        }
        if (pos.empty()) throw UsageError("extract needs an archive");
        o.archive = pos[0];
        o.paths.assign(pos.begin() + 1, pos.end());
        return cmd_extract(o);
    }
    if (cmd == "list" || cmd == "l") {
        bool long_format = false;
        for (size_t i = 1; i < a.size(); ++i) {
            if (a[i] == "-l" || a[i] == "--long") long_format = true;
            else if (a[i].size() > 1 && a[i][0] == '-') throw UsageError("unknown option: " + a[i]);
            else pos.push_back(a[i]);
        }
        if (pos.size() != 1) throw UsageError("list needs exactly one archive");
        return cmd_list(pos[0], long_format);
    }
    if (cmd == "test" || cmd == "t") {
        unsigned threads = default_threads();
        for (size_t i = 1; i < a.size(); ++i) {
            if (a[i] == "-j" || a[i] == "--threads") threads = parse_threads(need_value(a, i));
            else if (a[i].size() > 1 && a[i][0] == '-') throw UsageError("unknown option: " + a[i]);
            else pos.push_back(a[i]);
        }
        if (pos.size() != 1) throw UsageError("test needs exactly one archive");
        return cmd_test(pos[0], threads);
    }
    if (cmd == "export-zip") {
        ZipOptions o;
        for (size_t i = 1; i < a.size(); ++i) {
            if (a[i] == "-f" || a[i] == "--force") o.force = true;
            else if (a[i] == "--level") {
                long n = std::strtol(need_value(a, i).c_str(), nullptr, 10);
                if (n < 1 || n > 9) throw UsageError("--level must be 1-9");
                o.level = static_cast<int>(n);
            }
            else if (a[i].size() > 1 && a[i][0] == '-') throw UsageError("unknown option: " + a[i]);
            else pos.push_back(a[i]);
        }
        if (pos.size() != 2) throw UsageError("export-zip needs <archive.mzg> <out.zip>");
        o.archive = pos[0];
        o.zip_path = pos[1];
        return cmd_export_zip(o);
    }
    throw UsageError("unknown command: " + cmd);
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    try {
        return run(args);
    } catch (const UsageError& e) {
        std::fprintf(stderr, "zzipp: %s\n\n", e.what());
        print_usage(stderr);
        return 2;
    } catch (const std::invalid_argument& e) {
        std::fprintf(stderr, "zzipp: %s\n", e.what());
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "zzipp: error: %s\n", e.what());
        return 1;
    }
}
