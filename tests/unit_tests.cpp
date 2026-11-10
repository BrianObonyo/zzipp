// Dependency-free unit tests. Run via `ctest` or ./zzipp_tests.
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "archive.hpp"
#include "bytes.hpp"
#include "codec.hpp"
#include "format.hpp"
#include "util.hpp"

using namespace zzipp;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

template <class Fn>
static bool throws_format_error(Fn&& fn) {
    try {
        fn();
    } catch (const FormatError&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

static void test_bytes() {
    ByteWriter w;
    w.u8(7); w.u16(0xBEEF); w.u32(0xDEADBEEF); w.u64(0x0123456789ABCDEFull); w.i64(-42); w.str("héllo");
    ByteReader r(w.bytes().data(), w.bytes().size());
    CHECK(r.u8() == 7);
    CHECK(r.u16() == 0xBEEF);
    CHECK(r.u32() == 0xDEADBEEF);
    CHECK(r.u64() == 0x0123456789ABCDEFull);
    CHECK(r.i64() == -42);
    CHECK(r.str() == "héllo");
    CHECK(r.remaining() == 0);
    CHECK(throws_format_error([&] { r.u8(); }));

    // A string claiming to be longer than the buffer must be rejected, not read.
    ByteWriter bad;
    bad.u32(0xFFFFFFFF);
    ByteReader rb(bad.bytes().data(), bad.bytes().size());
    CHECK(throws_format_error([&] { rb.str(); }));
}

static void test_util() {
    CHECK(parse_size("4096") == 4096);
    CHECK(parse_size("64m") == 64 * MiB);
    CHECK(parse_size("1G") == GiB);
    CHECK(parse_size("512KiB") == 512 * KiB);
    bool threw = false;
    try { parse_size("abc"); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { parse_size("10x"); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    CHECK(group_digits(1234567) == "1,234,567");
    CHECK(group_digits(12) == "12");
}

static void test_paths() {
    CHECK(is_safe_relative_path("a/b/c.txt"));
    CHECK(is_safe_relative_path("dir with spaces/file"));
    CHECK(!is_safe_relative_path(""));
    CHECK(!is_safe_relative_path("/etc/passwd"));
    CHECK(!is_safe_relative_path("../evil"));
    CHECK(!is_safe_relative_path("a/../../evil"));
    CHECK(!is_safe_relative_path("a//b"));
    CHECK(!is_safe_relative_path("a/./b"));
    CHECK(!is_safe_relative_path("a/"));
    CHECK(!is_safe_relative_path("a\\b"));
    CHECK(!is_safe_relative_path(std::string("a\0b", 3)));
}

static void test_codec() {
    std::mt19937 rng(1234);
    // Compressible text.
    std::string text;
    for (int i = 0; i < 5000; ++i) text += "the quick brown fox " + std::to_string(i % 17) + "\n";
    std::vector<uint8_t> in(text.begin(), text.end()), out;
    uint32_t dict = 0;
    CHECK(lzma2_encode(in.data(), in.size(), false, 6, 0, out, dict));
    CHECK(out.size() < in.size() / 4);
    std::vector<uint8_t> back(in.size());
    lzma2_decode(out.data(), out.size(), Method::Lzma2, dict, back.data(), back.size());
    CHECK(back == in);

    // x86 call-like patterns through the BCJ filter must round-trip exactly.
    std::vector<uint8_t> code;
    for (int i = 0; i < 4000; ++i) {
        code.push_back(0xE8);
        uint32_t rel = static_cast<uint32_t>(rng() % 5000);
        for (int b = 0; b < 4; ++b) code.push_back(static_cast<uint8_t>(rel >> (8 * b)));
        code.push_back(0x90);
    }
    CHECK(lzma2_encode(code.data(), code.size(), true, 6, 0, out, dict));
    back.assign(code.size(), 0);
    lzma2_decode(out.data(), out.size(), Method::Lzma2Bcj86, dict, back.data(), back.size());
    CHECK(back == code);

    // Random data must be reported incompressible so the caller stores it.
    std::vector<uint8_t> noise(100000);
    for (auto& b : noise) b = static_cast<uint8_t>(rng());
    CHECK(!lzma2_encode(noise.data(), noise.size(), false, 6, 0, out, dict));

    // Corrupt / truncated streams must throw FormatError, never crash.
    std::vector<uint8_t> good;
    CHECK(lzma2_encode(in.data(), in.size(), false, 6, 0, good, dict));
    for (int i = 0; i < 300; ++i) {
        std::vector<uint8_t> bad = good;
        bad[rng() % bad.size()] ^= static_cast<uint8_t>(1 + rng() % 255);
        std::vector<uint8_t> dst(in.size());
        try {
            lzma2_decode(bad.data(), bad.size(), Method::Lzma2, dict, dst.data(), dst.size());
        } catch (const FormatError&) {
        }
    }
    std::vector<uint8_t> half(good.begin(), good.begin() + good.size() / 2), dst(in.size());
    CHECK(throws_format_error([&] { lzma2_decode(half.data(), half.size(), Method::Lzma2, dict, dst.data(), dst.size()); }));
}

static Index sample_index() {
    Index idx;
    BlockInfo b;
    b.offset = 32; b.comp_size = 100; b.raw_size = 400; b.comp_crc = 0x1234; b.dict_size = 8192;
    b.method = Method::Lzma2;
    idx.blocks.push_back(b);
    Entry d; d.kind = EntryKind::Dir; d.path = "proj"; d.mtime = 1700000000; d.mode = 0755;
    Entry f1; f1.path = "proj/a.txt"; f1.size = 100; f1.block = 0; f1.offset = 0; f1.crc = 1; f1.mode = 0644;
    Entry f2; f2.path = "proj/b.txt"; f2.size = 300; f2.block = 0; f2.offset = 100; f2.crc = 2; f2.mode = 0644;
    Entry e; e.path = "proj/empty"; e.size = 0; e.block = NO_BLOCK;
    idx.entries = {d, f1, f2, e};
    return idx;
}

static void test_index() {
    const Index idx = sample_index();
    const auto bytes = serialize_index(idx);
    const Index back = parse_index(bytes.data(), bytes.size());
    CHECK(back.blocks.size() == 1 && back.entries.size() == 4);
    CHECK(back.entries[2].path == "proj/b.txt" && back.entries[2].offset == 100);

    Header h; h.index_offset = 1234; h.index_size = 56; h.index_crc = 0xABCD;
    const auto hb = serialize_header(h);
    const Header hh = parse_header(hb.data(), hb.size());
    CHECK(hh.index_offset == 1234 && hh.index_size == 56 && hh.index_crc == 0xABCD);
    auto bad_magic = hb;
    bad_magic[0] = 'X';
    CHECK(throws_format_error([&] { parse_header(bad_magic.data(), bad_magic.size()); }));

    // Semantic validation.
    Index evil = sample_index();
    evil.entries[1].path = "../escape";
    auto eb = serialize_index(evil);
    CHECK(throws_format_error([&] { parse_index(eb.data(), eb.size()); }));

    evil = sample_index();
    evil.entries[2].offset = 350;  // 350 + 300 > raw_size 400
    eb = serialize_index(evil);
    CHECK(throws_format_error([&] { parse_index(eb.data(), eb.size()); }));

    evil = sample_index();
    evil.entries[1].block = 9;
    eb = serialize_index(evil);
    CHECK(throws_format_error([&] { parse_index(eb.data(), eb.size()); }));

    evil = sample_index();
    evil.entries[3].path = "proj/a.txt";  // duplicate path
    eb = serialize_index(evil);
    CHECK(throws_format_error([&] { parse_index(eb.data(), eb.size()); }));

    // Every truncation must be rejected.
    for (size_t n = 0; n < bytes.size(); ++n)
        CHECK(throws_format_error([&] { parse_index(bytes.data(), n); }));

    // Random mutation: parser must either accept or throw FormatError. Nothing else.
    std::mt19937 rng(99);
    for (int i = 0; i < 20000; ++i) {
        auto m = bytes;
        int flips = 1 + static_cast<int>(rng() % 3);
        for (int k = 0; k < flips; ++k) m[rng() % m.size()] = static_cast<uint8_t>(rng());
        try {
            parse_index(m.data(), m.size());
        } catch (const FormatError&) {
        } catch (...) {
            CHECK(!"parse_index threw something other than FormatError");
        }
    }
}

static void test_index_region() {
    Index idx = sample_index();
    // make it large enough that compression pays off
    for (int i = 0; i < 2000; ++i) {
        Entry e; e.path = "proj/generated/file_" + std::to_string(i) + ".txt"; e.size = 1; e.block = 0; e.offset = 0;
        idx.entries.push_back(e);
    }
    const auto region = encode_index_region(idx);
    CHECK(region[0] == static_cast<uint8_t>(Method::Lzma2));
    CHECK(region.size() < serialize_index(idx).size() / 3);
    const Index back = decode_index_region(region.data(), region.size());
    CHECK(back.entries.size() == idx.entries.size() && back.entries.back().path == idx.entries.back().path);
    for (size_t n = 0; n < 40; ++n)
        CHECK(throws_format_error([&] { decode_index_region(region.data(), n); }));
    std::mt19937 rng(7);
    for (int i = 0; i < 2000; ++i) {
        auto m = region;
        m[rng() % m.size()] ^= static_cast<uint8_t>(1 + rng() % 255);
        try { decode_index_region(m.data(), m.size()); } catch (const FormatError&) {}
        catch (...) { CHECK(!"decode_index_region threw something other than FormatError"); }
    }
}

int main() {
    test_bytes();
    test_util();
    test_paths();
    test_codec();
    test_index();
    test_index_region();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all unit tests passed");
    return 0;
}
