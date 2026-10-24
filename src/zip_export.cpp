// `zzipp export-zip`: converts an archive into a standard .zip that any tool can open.
// Classic ZIP only (no ZIP64): limited to 65,535 entries and 4 GiB per file/archive.
#include <zlib.h>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <stdexcept>
#include <system_error>

#include "archive.hpp"
#include "commands.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace zzipp {
namespace {

struct CentralEntry {
    std::string name;
    uint16_t method = 0, dos_time = 0, dos_date = 0;
    uint32_t crc = 0, comp_size = 0, size = 0, ext_attr = 0, local_offset = 0;
};

void dos_datetime(int64_t unix_time, uint16_t& t, uint16_t& d) {
    std::time_t tt = static_cast<std::time_t>(unix_time);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    int year = tmv.tm_year + 1900;
    if (year < 1980 || year > 2107) {
        t = 0;
        d = static_cast<uint16_t>((1 << 5) | 1);  // 1980-01-01 00:00
        return;
    }
    t = static_cast<uint16_t>((tmv.tm_hour << 11) | (tmv.tm_min << 5) | (tmv.tm_sec / 2));
    d = static_cast<uint16_t>(((year - 1980) << 9) | ((tmv.tm_mon + 1) << 5) | tmv.tm_mday);
}

std::vector<uint8_t> deflate_raw(const uint8_t* in, size_t n, int level) {
    z_stream zs{};
    if (deflateInit2(&zs, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw std::runtime_error("zlib init failed");
    std::vector<uint8_t> out(deflateBound(&zs, static_cast<uLong>(n)));
    zs.next_in = const_cast<Bytef*>(in);
    zs.avail_in = static_cast<uInt>(n);
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    int r = deflate(&zs, Z_FINISH);
    if (r != Z_STREAM_END) {
        deflateEnd(&zs);
        throw std::runtime_error("zlib deflate failed");
    }
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
}

class ZipWriter {
public:
    explicit ZipWriter(const fs::path& path) : out_(path, std::ios::binary | std::ios::trunc) {
        if (!out_) throw std::runtime_error("cannot write " + path.string());
    }

    void add(const Entry& e, const uint8_t* data, int level) {
        const bool dir = e.kind == EntryKind::Dir;
        CentralEntry c;
        c.name = e.path + (dir ? "/" : "");
        if (c.name.size() > 0xFFFF) throw std::runtime_error("path too long for ZIP: " + e.path);
        if (e.size >= 0xFFFFFFFFull) throw std::runtime_error("file too large for classic ZIP: " + e.path);
        dos_datetime(e.mtime, c.dos_time, c.dos_date);
        c.crc = e.crc;
        c.size = static_cast<uint32_t>(e.size);
        const uint32_t unix_type = dir ? 040000u : 0100000u;
        c.ext_attr = ((unix_type | (e.mode & 0777)) << 16) | (dir ? 0x10u : 0u);

        std::vector<uint8_t> packed;
        const uint8_t* body = data;
        c.comp_size = c.size;
        if (!dir && e.size > 0) {
            packed = deflate_raw(data, static_cast<size_t>(e.size), level);
            if (packed.size() < e.size) {
                c.method = 8;
                body = packed.data();
                c.comp_size = static_cast<uint32_t>(packed.size());
            }
        }
        const uint64_t pos = static_cast<uint64_t>(out_.tellp());
        if (pos >= 0xFFFFFFFFull) throw std::runtime_error("archive too large for classic ZIP");
        c.local_offset = static_cast<uint32_t>(pos);

        ByteWriter w;
        w.u32(0x04034b50);
        w.u16(20);
        w.u16(0x0800);  // UTF-8 names
        w.u16(c.method);
        w.u16(c.dos_time);
        w.u16(c.dos_date);
        w.u32(c.crc);
        w.u32(c.comp_size);
        w.u32(c.size);
        w.u16(static_cast<uint16_t>(c.name.size()));
        w.u16(0);
        w.raw(c.name);
        out_.write(reinterpret_cast<const char*>(w.bytes().data()), static_cast<std::streamsize>(w.bytes().size()));
        if (c.comp_size) out_.write(reinterpret_cast<const char*>(body), c.comp_size);
        if (!out_) throw std::runtime_error("write failed (disk full?)");
        central_.push_back(std::move(c));
        if (central_.size() > 0xFFFF) throw std::runtime_error("too many entries for classic ZIP");
    }

    void finish() {
        const uint64_t cd_start = static_cast<uint64_t>(out_.tellp());
        ByteWriter w;
        for (const auto& c : central_) {
            w.u32(0x02014b50);
            w.u16((3 << 8) | 20);  // made by: Unix, spec 2.0
            w.u16(20);
            w.u16(0x0800);
            w.u16(c.method);
            w.u16(c.dos_time);
            w.u16(c.dos_date);
            w.u32(c.crc);
            w.u32(c.comp_size);
            w.u32(c.size);
            w.u16(static_cast<uint16_t>(c.name.size()));
            w.u16(0);  // extra
            w.u16(0);  // comment
            w.u16(0);  // disk
            w.u16(0);  // internal attrs
            w.u32(c.ext_attr);
            w.u32(c.local_offset);
            w.raw(c.name);
        }
        const uint64_t cd_size = w.bytes().size();
        if (cd_start + cd_size >= 0xFFFFFFFFull) throw std::runtime_error("archive too large for classic ZIP");
        w.u32(0x06054b50);
        w.u16(0);
        w.u16(0);
        w.u16(static_cast<uint16_t>(central_.size()));
        w.u16(static_cast<uint16_t>(central_.size()));
        w.u32(static_cast<uint32_t>(cd_size));
        w.u32(static_cast<uint32_t>(cd_start));
        w.u16(0);
        out_.write(reinterpret_cast<const char*>(w.bytes().data()), static_cast<std::streamsize>(w.bytes().size()));
        out_.close();
        if (!out_) throw std::runtime_error("failed to finalise ZIP");
    }

    size_t count() const { return central_.size(); }

private:
    std::ofstream out_;
    std::vector<CentralEntry> central_;
};

}  // namespace

int cmd_export_zip(const ZipOptions& o) {
    Timer timer;
    ArchiveReader ar(o.archive);
    const Index& idx = ar.index();
    const fs::path zip_out = fs::u8path(o.zip_path);
    if (fs::exists(zip_out) && !o.force)
        throw std::runtime_error("output exists (use -f to overwrite): " + o.zip_path);

    std::random_device random;
    std::error_code ec;
    fs::path temp_dir;
    bool created_temp_dir = false;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const uint64_t nonce = (static_cast<uint64_t>(random()) << 32) | random();
        temp_dir = zip_out;
        temp_dir += ".tmp-" + std::to_string(nonce);
        ec.clear();
        if (fs::create_directory(temp_dir, ec)) {
            created_temp_dir = true;
            break;
        }
        if (ec && ec != std::errc::file_exists)
            throw std::runtime_error("cannot create temporary ZIP directory: " + ec.message());
    }
    if (!created_temp_dir) throw std::runtime_error("cannot create unique temporary ZIP directory");
    const fs::path tmp = temp_dir / "archive.zip";
    size_t zip_entries = 0;
    try {
        ZipWriter zip(tmp);
        std::map<uint32_t, std::vector<const Entry*>> by_block;
        for (const Entry& e : idx.entries) {
            if (e.kind == EntryKind::Dir || (e.kind == EntryKind::File && e.block == NO_BLOCK))
                zip.add(e, nullptr, o.level);  // directories and empty files
            else
                by_block[e.block].push_back(&e);
        }
        uint64_t done = 0, total = 0;
        for (const auto& kv : by_block) total += idx.blocks[kv.first].raw_size;
        o.cb.report(0, total);
        for (const auto& [b, entries] : by_block) {
            o.cb.check();
            std::vector<uint8_t> raw = ar.read_block(b);
            for (const Entry* e : entries) {
                if (crc32(raw.data() + e->offset, static_cast<size_t>(e->size)) != e->crc)
                    throw FormatError("checksum mismatch in file: " + e->path);
                zip.add(*e, raw.data() + e->offset, o.level);
            }
            done += raw.size();
            o.cb.report(done, total);
        }
        zip.finish();
        zip_entries = zip.count();
    } catch (...) {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
        throw;
    }
    ec.clear();
    if (o.force) fs::remove(zip_out, ec);
    if (ec) {
        const std::string message = ec.message();
        fs::remove_all(temp_dir, ec);
        throw std::runtime_error("cannot remove existing ZIP: " + message);
    }
    ec.clear();
    fs::rename(tmp, zip_out, ec);
    if (ec) {
        const std::string message = ec.message();
        fs::remove_all(temp_dir, ec);
        throw std::runtime_error("cannot move ZIP into place: " + message);
    }
    ec.clear();
    fs::remove(temp_dir, ec);
    if (ec) throw std::runtime_error("ZIP created but cannot remove temporary directory: " + ec.message());
    o.cb.say(format_string("%s: %zu entries in %.2fs", o.zip_path.c_str(), zip_entries, timer.seconds()));
    return 0;
}

}  // namespace zzipp
