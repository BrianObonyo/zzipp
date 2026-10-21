// `zzipp create`: scan -> dedup -> classify/sort -> pack into solid blocks ->
// compress blocks in parallel -> write blocks in order -> write index + header.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include "archive.hpp"
#include "classify.hpp"
#include "codec.hpp"
#include "commands.hpp"
#include "format.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace zzipp {
namespace {

constexpr size_t NPOS = static_cast<size_t>(-1);

struct FileItem {
    fs::path abs;
    std::string rel, ext;
    uint64_t size = 0;
    int64_t mtime = 0;
    uint32_t mode = 0;
    FileClass cls = FileClass::Normal;
    size_t dup_of = NPOS;  // index of the identical file whose data we reuse
    uint32_t crc = 0;
    uint32_t block = NO_BLOCK;
    uint64_t offset = 0;
};

struct DirItem {
    std::string rel;
    int64_t mtime = 0;
    uint32_t mode = 0;
};

struct Scan {
    std::vector<FileItem> files;
    std::vector<DirItem> dirs;
};

struct BlockPlan {
    FileClass cls = FileClass::Normal;
    std::vector<size_t> items;
    uint64_t raw = 0;
};

struct BlockResult {
    std::vector<uint8_t> data;
    BlockInfo info;
    bool ready = false;
};

void scan_input(const std::string& input, const fs::path& skip_a, const fs::path& skip_b, Scan& out,
                const Callbacks& cb) {
    fs::path p = fs::absolute(fs::u8path(input)).lexically_normal();
    if (!p.has_filename()) p = p.parent_path();  // "dir/" -> "dir"
    std::error_code ec;
    auto st = fs::symlink_status(p, ec);
    if (ec || !fs::exists(st)) throw std::runtime_error("cannot access: " + input);
    const fs::path base = p.parent_path();

    auto handle = [&](const fs::path& q, const fs::file_status& qs) {
        if (q == skip_a || q == skip_b) return;  // never archive the archive itself
        if (fs::is_symlink(qs)) {
            cb.warn("skipping symlink " + q.string());
            return;
        }
        const std::string rel = q.lexically_relative(base).generic_u8string();
        if (rel.empty() || !is_safe_relative_path(rel))
            throw std::runtime_error("unsupported path (cannot archive): " + q.string());
        std::error_code e;
        auto mt = fs::last_write_time(q, e);
        const int64_t mtime = e ? 0 : to_unix_time(mt);
        const uint32_t mode = static_cast<uint32_t>(qs.permissions()) & 0777;
        if (fs::is_directory(qs)) {
            out.dirs.push_back({rel, mtime, mode});
        } else if (fs::is_regular_file(qs)) {
            FileItem f;
            f.abs = q;
            f.rel = rel;
            f.ext = lowercase_extension(q);
            f.size = fs::file_size(q, e);
            if (e) throw std::runtime_error("cannot stat " + q.string());
            f.mtime = mtime;
            f.mode = mode;
            f.cls = f.size ? classify_file(q) : FileClass::Normal;
            out.files.push_back(std::move(f));
        } else {
            cb.warn("skipping special file " + q.string());
        }
    };

    handle(p, st);
    if (fs::is_directory(st)) {
        fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end;
        if (ec) throw std::runtime_error("cannot read directory " + p.string() + ": " + ec.message());
        for (; it != end; it.increment(ec)) {
            if (ec) throw std::runtime_error("directory walk failed: " + ec.message());
            std::error_code e2;
            auto qs = it->symlink_status(e2);
            if (e2) throw std::runtime_error("cannot stat " + it->path().string());
            handle(it->path(), qs);
        }
    }
}

uint64_t hash_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + p.string());
    std::vector<uint8_t> buf(1 << 20);
    uint64_t h = 0;
    while (in) {
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        h = crc64(buf.data(), static_cast<size_t>(in.gcount()), h);
    }
    return h;
}

bool files_equal(const fs::path& a, const fs::path& b) {
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) throw std::runtime_error("cannot open " + (fa ? b : a).string());
    std::vector<char> x(1 << 18), y(1 << 18);
    for (;;) {
        fa.read(x.data(), static_cast<std::streamsize>(x.size()));
        fb.read(y.data(), static_cast<std::streamsize>(y.size()));
        if (fa.gcount() != fb.gcount()) return false;
        if (fa.gcount() == 0) return true;
        if (std::memcmp(x.data(), y.data(), static_cast<size_t>(fa.gcount())) != 0) return false;
    }
}

// Files with a unique size cannot have duplicates, so only same-size files are hashed.
// A hash match is always confirmed with a byte-for-byte comparison.
size_t find_duplicates(std::vector<FileItem>& files, const Callbacks& cb) {
    std::unordered_map<uint64_t, std::vector<size_t>> by_size;
    for (size_t i = 0; i < files.size(); ++i)
        if (files[i].size > 0) by_size[files[i].size].push_back(i);
    size_t dups = 0;
    for (auto& [size, ids] : by_size) {
        (void)size;
        if (ids.size() < 2) continue;
        std::unordered_map<uint64_t, std::vector<size_t>> canon;
        for (size_t i : ids) {
            cb.check();
            auto& list = canon[hash_file(files[i].abs)];
            bool dup = false;
            for (size_t c : list) {
                if (files_equal(files[c].abs, files[i].abs)) {
                    files[i].dup_of = c;
                    dup = true;
                    ++dups;
                    break;
                }
            }
            if (!dup) list.push_back(i);
        }
    }
    return dups;
}

std::vector<BlockPlan> plan_blocks(const std::vector<FileItem>& files, const CreateOptions& o) {
    std::vector<size_t> order;
    for (size_t i = 0; i < files.size(); ++i)
        if (files[i].size > 0 && files[i].dup_of == NPOS) order.push_back(i);

    auto rank = [](FileClass c) { return c == FileClass::Normal ? 0 : c == FileClass::Exec ? 1 : 2; };
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const FileItem &A = files[a], &B = files[b];
        if (A.cls != B.cls) return rank(A.cls) < rank(B.cls);
        if (o.sort && A.ext != B.ext) return A.ext < B.ext;  // similar files end up adjacent
        return A.rel < B.rel;
    });

    std::vector<BlockPlan> plans;
    for (size_t i : order) {
        const FileItem& f = files[i];
        bool fresh = plans.empty() || plans.back().cls != f.cls ||
                     (plans.back().raw > 0 && plans.back().raw + f.size > o.block);
        if (fresh) plans.push_back(BlockPlan{f.cls, {}, 0});
        plans.back().items.push_back(i);
        plans.back().raw += f.size;
    }
    return plans;
}

void read_exact(const fs::path& p, uint8_t* dst, uint64_t n) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + p.string());
    in.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(n));
    if (static_cast<uint64_t>(in.gcount()) != n)
        throw std::runtime_error("file shrank while archiving: " + p.string());
    char extra;
    if (in.read(&extra, 1)) throw std::runtime_error("file grew while archiving: " + p.string());
}

BlockResult compress_block(uint32_t block_idx, const BlockPlan& plan, std::vector<FileItem>& files,
                           const CreateOptions& o) {
    std::vector<uint8_t> raw(plan.raw);
    uint64_t off = 0;
    for (size_t idx : plan.items) {
        FileItem& f = files[idx];  // each file belongs to exactly one block: no data race
        read_exact(f.abs, raw.data() + off, f.size);
        f.crc = crc32(raw.data() + off, f.size);
        f.block = block_idx;
        f.offset = off;
        off += f.size;
    }

    BlockResult r;
    r.info.raw_size = plan.raw;
    bool packed = false;
    if (plan.cls != FileClass::Store) {
        const bool bcj = plan.cls == FileClass::Exec;
        uint32_t dict = 0;
        if (lzma2_encode(raw.data(), raw.size(), bcj, o.preset, o.dict, r.data, dict)) {
            r.info.method = bcj ? Method::Lzma2Bcj86 : Method::Lzma2;
            r.info.dict_size = dict;
            packed = true;
        }
    }
    if (!packed) {  // already-compressed or incompressible: keep the original bytes
        r.data = std::move(raw);
        r.info.method = Method::Store;
        r.info.dict_size = 0;
    }
    r.info.comp_size = r.data.size();
    r.info.comp_crc = crc32(r.data.data(), r.data.size());
    return r;
}

struct PoolGuard {
    std::vector<std::thread> threads;
    std::atomic<bool>& abort;
    ~PoolGuard() {
        abort = true;
        for (auto& t : threads)
            if (t.joinable()) t.join();
    }
};

}  // namespace

int cmd_create(const CreateOptions& o) {
    Timer timer;
    o.cb.report(0, 0);  // scanning: progress unknown
    const fs::path out_path = fs::absolute(fs::u8path(o.archive)).lexically_normal();
    fs::path tmp_path = out_path;
    tmp_path += ".partial";
    if (fs::exists(out_path) && !o.force)
        throw std::runtime_error("archive already exists (use -f to overwrite): " + o.archive);

    Scan scan;
    for (const auto& in : o.inputs) {
        o.cb.check();
        scan_input(in, out_path, tmp_path, scan, o.cb);
    }

    std::sort(scan.files.begin(), scan.files.end(),
              [](const FileItem& a, const FileItem& b) { return a.rel < b.rel; });
    std::sort(scan.dirs.begin(), scan.dirs.end(),
              [](const DirItem& a, const DirItem& b) { return a.rel < b.rel; });
    {
        std::set<std::string> seen;
        for (const auto& f : scan.files)
            if (!seen.insert(f.rel).second) throw std::runtime_error("duplicate path in input: " + f.rel);
        for (const auto& d : scan.dirs)
            if (!seen.insert(d.rel).second) throw std::runtime_error("duplicate path in input: " + d.rel);
    }
    if (scan.files.empty() && scan.dirs.empty()) throw std::runtime_error("nothing to archive");

    const size_t dups = o.dedup ? find_duplicates(scan.files, o.cb) : 0;
    const std::vector<BlockPlan> plans = plan_blocks(scan.files, o);

    uint64_t total_in = 0;
    for (const auto& f : scan.files) total_in += f.size;

    // ---- compress blocks on worker threads, write them in order on this thread ----
    std::vector<BlockResult> results(plans.size());
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<size_t> next{0};
    std::atomic<bool> abort{false};
    std::exception_ptr error;

    auto worker = [&] {
        for (;;) {
            if (abort.load()) return;
            size_t i = next.fetch_add(1);
            if (i >= plans.size()) return;
            try {
                BlockResult r = compress_block(static_cast<uint32_t>(i), plans[i], scan.files, o);
                std::lock_guard<std::mutex> lk(mu);
                results[i] = std::move(r);
                results[i].ready = true;
                cv.notify_all();
            } catch (...) {
                std::lock_guard<std::mutex> lk(mu);
                if (!error) error = std::current_exception();
                abort = true;
                cv.notify_all();
                return;
            }
        }
    };

    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create output file " + out_path.string() +
                                 " (does the folder exist, and can you write to it?)");
    const std::array<uint8_t, HEADER_SIZE> zero_header{};
    out.write(reinterpret_cast<const char*>(zero_header.data()), HEADER_SIZE);

    Index idx;
    uint64_t pos = HEADER_SIZE;
    uint64_t done_raw = 0, total_raw = 0;
    for (const auto& p : plans) total_raw += p.raw;
    try {
        PoolGuard guard{{}, abort};
        const unsigned nthreads =
            static_cast<unsigned>(std::max<size_t>(1, std::min<size_t>(o.threads, plans.size())));
        for (unsigned t = 0; t < nthreads; ++t) guard.threads.emplace_back(worker);

        for (size_t i = 0; i < plans.size(); ++i) {
            BlockResult r;
            {
                std::unique_lock<std::mutex> lk(mu);
                while (!(results[i].ready || error)) {
                    cv.wait_for(lk, std::chrono::milliseconds(100));
                    if (o.cb.cancelled()) throw Cancelled();
                }
                if (error) std::rethrow_exception(error);
                r = std::move(results[i]);
            }
            r.info.offset = pos;
            out.write(reinterpret_cast<const char*>(r.data.data()), static_cast<std::streamsize>(r.data.size()));
            if (!out) throw std::runtime_error("write failed (disk full?)");
            pos += r.data.size();
            if (o.verbose) {
                static const char* names[] = {"store", "lzma2", "bcj+lzma2"};
                std::fprintf(stderr, "  block %zu/%zu: %s -> %s [%s]\n", i + 1, plans.size(),
                             human_size(r.info.raw_size).c_str(), human_size(r.info.comp_size).c_str(),
                             names[static_cast<int>(r.info.method)]);
            }
            idx.blocks.push_back(r.info);
            done_raw += r.info.raw_size;
            o.cb.report(done_raw, total_raw);
        }
    } catch (...) {
        out.close();
        std::error_code ec;
        fs::remove(tmp_path, ec);
        throw;
    }

    // ---- index ----
    for (auto& f : scan.files) {
        if (f.dup_of != NPOS) {
            const FileItem& c = scan.files[f.dup_of];
            f.crc = c.crc;
            f.block = c.block;
            f.offset = c.offset;
        }
    }
    for (const auto& d : scan.dirs) {
        Entry e;
        e.kind = EntryKind::Dir;
        e.path = d.rel;
        e.mtime = d.mtime;
        e.mode = d.mode;
        idx.entries.push_back(std::move(e));
    }
    for (const auto& f : scan.files) {
        Entry e;
        e.kind = EntryKind::File;
        e.path = f.rel;
        e.size = f.size;
        e.mtime = f.mtime;
        e.mode = f.mode;
        e.crc = f.crc;
        e.block = f.block;
        e.offset = f.offset;
        idx.entries.push_back(std::move(e));
    }
    std::sort(idx.entries.begin(), idx.entries.end(),
              [](const Entry& a, const Entry& b) { return a.path < b.path; });

    const std::vector<uint8_t> index_bytes = encode_index_region(idx);
    Header h;
    h.index_offset = pos;
    h.index_size = index_bytes.size();
    h.index_crc = crc32(index_bytes.data(), index_bytes.size());
    out.write(reinterpret_cast<const char*>(index_bytes.data()), static_cast<std::streamsize>(index_bytes.size()));
    pos += index_bytes.size();
    out.seekp(0);
    const auto header = serialize_header(h);
    out.write(reinterpret_cast<const char*>(header.data()), HEADER_SIZE);
    out.close();
    if (!out) throw std::runtime_error("failed to finalise archive");

    std::error_code ec;
    if (o.force) fs::remove(out_path, ec);
    fs::rename(tmp_path, out_path, ec);
    if (ec) throw std::runtime_error("cannot move archive into place: " + ec.message());

    // ---- summary ----
    size_t n_method[3] = {0, 0, 0};
    for (const auto& b : idx.blocks) ++n_method[static_cast<int>(b.method)];
    const double secs = timer.seconds();
    std::string head = format_string("%s: %s files, %s dirs", o.archive.c_str(),
                                     group_digits(scan.files.size()).c_str(), group_digits(scan.dirs.size()).c_str());
    if (dups) head += format_string(" (%zu duplicate%s stored once)", dups, dups == 1 ? "" : "s");
    o.cb.say(head);
    o.cb.say(format_string("  input : %s", human_size(total_in).c_str()));
    o.cb.say(format_string("  output: %s (%.1f%% of input)", human_size(pos).c_str(),
                           total_in ? 100.0 * static_cast<double>(pos) / static_cast<double>(total_in) : 100.0));
    o.cb.say(format_string("  blocks: %zu lzma2, %zu bcj+lzma2, %zu stored", n_method[1], n_method[2], n_method[0]));
    o.cb.say(format_string("  time  : %.2fs (%.1f MiB/s)", secs,
                           secs > 0 ? static_cast<double>(total_in) / static_cast<double>(MiB) / secs : 0.0));
    return 0;
}

}  // namespace zzipp
