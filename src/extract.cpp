// `zzipp extract`, `list` and `test`.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include "archive.hpp"
#include "commands.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace zzipp {
namespace {

struct ErrorSink {
    std::mutex mu;
    std::vector<std::string> messages;
    void add(std::string m) {
        std::lock_guard<std::mutex> lk(mu);
        messages.push_back(std::move(m));
    }
};

std::string normalize_selector(std::string s) {
    while (s.rfind("./", 0) == 0) s.erase(0, 2);
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

bool matches(const std::string& rel, const std::string& sel) {
    return rel == sel || (rel.size() > sel.size() && rel.compare(0, sel.size(), sel) == 0 && rel[sel.size()] == '/');
}

void apply_metadata(const fs::path& p, const Entry& e) {
    std::error_code ec;
    if (e.mode) fs::permissions(p, static_cast<fs::perms>(e.mode & 0777), fs::perm_options::replace, ec);
    fs::last_write_time(p, from_unix_time(e.mtime), ec);  // best effort
}

// Writes one file's bytes. Returns false (and records an error) if it could not be written.
bool write_file(const fs::path& dest, const uint8_t* data, size_t n, const Entry& e, bool force, ErrorSink& errs) {
    std::error_code ec;
    auto st = fs::symlink_status(dest, ec);
    if (fs::exists(st)) {
        if (!force) {
            errs.add("exists, skipped (use -f to overwrite): " + e.path);
            return false;
        }
        if (fs::is_symlink(st) || fs::is_directory(st)) {
            errs.add("refusing to overwrite symlink/directory: " + e.path);
            return false;
        }
#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(dest.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) &&
            !SetFileAttributesW(dest.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY)) {
            errs.add("cannot make existing file writable: " + dest.string());
            return false;
        }
#endif
        if (!fs::remove(dest, ec) || ec) {
            errs.add("cannot remove existing file " + dest.string() + " (" + ec.message() + ")");
            return false;
        }
    }
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) {
        errs.add("cannot create " + dest.string() + " (" + std::strerror(errno) + ")");
        return false;
    }
    if (n) out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
    out.close();
    if (!out) {
        errs.add("write failed: " + dest.string());
        return false;
    }
    apply_metadata(dest, e);
    return true;
}

std::map<uint32_t, std::vector<const Entry*>> group_by_block(const std::vector<const Entry*>& files) {
    std::map<uint32_t, std::vector<const Entry*>> groups;
    for (const Entry* e : files)
        if (e->block != NO_BLOCK) groups[e->block].push_back(e);
    return groups;
}

fs::path under(const fs::path& root, const std::string& rel) { return root / fs::u8path(rel); }

void check_crc(const Entry& e, const std::vector<uint8_t>& raw) {
    if (crc32(raw.data() + e.offset, static_cast<size_t>(e.size)) != e.crc)
        throw FormatError("checksum mismatch in file: " + e.path);
}

}  // namespace

int cmd_list(const std::string& archive, bool long_format) {
    ArchiveReader ar(archive);
    const Index& idx = ar.index();
    uint64_t total = 0, files = 0, dirs = 0;
    if (long_format) std::printf("  Mode      Size  Modified (UTC)    CRC32     Path\n");
    for (const Entry& e : idx.entries) {
        const bool dir = e.kind == EntryKind::Dir;
        (dir ? dirs : files)++;
        total += e.size;
        if (!long_format) {
            std::printf("%s%s\n", e.path.c_str(), dir ? "/" : "");
            continue;
        }
        char when[32] = "-";
        std::time_t t = static_cast<std::time_t>(e.mtime);
        std::tm tmv{};
#ifdef _WIN32
        gmtime_s(&tmv, &t);
#else
        gmtime_r(&t, &tmv);
#endif
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tmv);
        std::printf("%c %04o %10llu  %s  %08x  %s%s\n", dir ? 'd' : '-', e.mode,
                    static_cast<unsigned long long>(e.size), when, e.crc, e.path.c_str(), dir ? "/" : "");
    }
    std::printf("\n%s files, %s dirs, %s total", group_digits(files).c_str(), group_digits(dirs).c_str(),
                human_size(total).c_str());
    if (total) std::printf("; archive is %s (%.1f%%)", human_size(ar.file_size()).c_str(),
                           100.0 * static_cast<double>(ar.file_size()) / static_cast<double>(total));
    std::printf("\n");
    return 0;
}

int cmd_test(const std::string& archive, unsigned threads, const Callbacks& cb) {
    ArchiveReader ar(archive);
    const Index& idx = ar.index();
    std::vector<const Entry*> files;
    for (const Entry& e : idx.entries)
        if (e.kind == EntryKind::File) files.push_back(&e);
    auto groups = group_by_block(files);

    std::atomic<uint64_t> blocks_done{0};
    parallel_for(idx.blocks.size(), threads, [&](size_t b) {
        cb.check();
        std::vector<uint8_t> raw = ar.read_block(static_cast<uint32_t>(b));  // verifies block CRC
        auto it = groups.find(static_cast<uint32_t>(b));
        if (it != groups.end())
            for (const Entry* e : it->second) check_crc(*e, raw);
        cb.report(++blocks_done, idx.blocks.size());
    });
    cb.say(format_string("OK: %zu blocks, %zu files verified", idx.blocks.size(), files.size()));
    return 0;
}

int cmd_extract(const ExtractOptions& o) {
    Timer timer;
    ArchiveReader ar(o.archive);
    const Index& idx = ar.index();

    std::vector<std::string> selectors;
    for (const auto& p : o.paths) selectors.push_back(normalize_selector(p));
    std::vector<bool> used(selectors.size(), false);

    std::vector<const Entry*> dirs, files;
    for (const Entry& e : idx.entries) {
        bool pick = selectors.empty();
        for (size_t i = 0; i < selectors.size(); ++i)
            if (matches(e.path, selectors[i])) {
                pick = true;
                used[i] = true;
            }
        if (!pick) continue;
        (e.kind == EntryKind::Dir ? dirs : files).push_back(&e);
    }
    for (size_t i = 0; i < selectors.size(); ++i)
        if (!used[i]) throw std::runtime_error("not found in archive: " + selectors[i]);

    const fs::path root = fs::u8path(o.out_dir);
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) throw std::runtime_error("cannot create output directory: " + ec.message());

    ErrorSink errs;
    for (const Entry* d : dirs) fs::create_directories(under(root, d->path), ec);
    for (const Entry* f : files) fs::create_directories((under(root, f->path)).parent_path(), ec);

    uint64_t bytes = 0, total_bytes = 0;
    size_t written = 0;
    for (const Entry* f : files) total_bytes += f->size;
    o.cb.report(0, total_bytes);
    std::mutex stat_mu;

    // Empty files have no data block.
    for (const Entry* f : files) {
        if (f->block != NO_BLOCK) continue;
        if (write_file(under(root, f->path), nullptr, 0, *f, o.force, errs)) ++written;
    }

    auto groups = group_by_block(files);
    std::vector<std::pair<uint32_t, std::vector<const Entry*>>> work(groups.begin(), groups.end());
    parallel_for(work.size(), o.threads, [&](size_t w) {
        o.cb.check();
        std::vector<uint8_t> raw = ar.read_block(work[w].first);
        for (const Entry* e : work[w].second) {
            check_crc(*e, raw);
            if (write_file(under(root, e->path), raw.data() + e->offset, static_cast<size_t>(e->size), *e, o.force, errs)) {
                std::lock_guard<std::mutex> lk(stat_mu);
                ++written;
                bytes += e->size;
                o.cb.report(bytes, total_bytes);
            }
            if (o.verbose) std::fprintf(stderr, "  %s\n", e->path.c_str());
        }
    });

    // Directory metadata last: writing files inside a directory changes its mtime.
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) apply_metadata(under(root, (*it)->path), **it);

    for (const auto& m : errs.messages) o.cb.warn(m);
    o.cb.say(format_string("extracted %zu file%s (%s) to %s in %.2fs", written, written == 1 ? "" : "s",
                           human_size(bytes).c_str(), o.out_dir.c_str(), timer.seconds()));
    return errs.messages.empty() ? 0 : 1;
}

}  // namespace zzipp
