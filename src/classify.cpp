#include "classify.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <string_view>
#include <unordered_set>

namespace fs = std::filesystem;

namespace zzipp {

namespace {

struct Magic {
    size_t offset;
    std::string_view bytes;
};

// Formats that are already compressed: recompressing only burns CPU.
const Magic kCompressedMagic[] = {
    {0, "\x89PNG"},          {0, "\xFF\xD8\xFF"},     {0, "GIF8"},       {0, "PK\x03\x04"},
    {0, "\x1F\x8B"},         {0, "\xFD" "7zXZ"},      {0, "7z\xBC\xAF"}, {0, "Rar!"},
    {0, "\x28\xB5\x2F\xFD"}, {0, "BZh"},              {4, "ftyp"},       {0, "OggS"},
    {0, "fLaC"},             {0, "ID3"},              {0, "MZG1"},       {0, "\x1A\x45\xDF\xA3"},
};

const std::unordered_set<std::string>& store_extensions() {
    static const std::unordered_set<std::string> s = {
        "jpg", "jpeg", "png", "gif", "webp", "heic", "heif", "avif", "mp3", "aac", "m4a", "ogg",
        "oga", "opus", "flac", "mp4", "m4v", "mkv", "webm", "avi", "mov", "zip", "gz", "tgz",
        "bz2", "xz", "zst", "7z", "rar", "lz4", "jar", "apk", "docx", "xlsx", "pptx", "odt",
        "ods", "odp", "woff2", "mzg"};
    return s;
}

const std::unordered_set<std::string>& exec_extensions() {
    static const std::unordered_set<std::string> s = {"exe", "dll", "sys", "ocx"};
    return s;
}

bool has_magic(const std::array<uint8_t, 32>& h, size_t got, const Magic& m) {
    if (m.offset + m.bytes.size() > got) return false;
    for (size_t i = 0; i < m.bytes.size(); ++i)
        if (h[m.offset + i] != static_cast<uint8_t>(m.bytes[i])) return false;
    return true;
}

}  // namespace

std::string lowercase_extension(const fs::path& path) {
    std::string e = path.extension().string();
    if (!e.empty() && e.front() == '.') e.erase(e.begin());
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

FileClass classify_file(const fs::path& path) {
    const std::string ext = lowercase_extension(path);

    std::array<uint8_t, 32> h{};
    size_t got = 0;
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            in.read(reinterpret_cast<char*>(h.data()), static_cast<std::streamsize>(h.size()));
            got = static_cast<size_t>(in.gcount());
        }
    }

    for (const auto& m : kCompressedMagic)
        if (has_magic(h, got, m)) return FileClass::Store;
    if (store_extensions().count(ext)) return FileClass::Store;

    // ELF: only x86 (3) and x86-64 (62) benefit from the x86 BCJ filter.
    if (got >= 20 && h[0] == 0x7F && h[1] == 'E' && h[2] == 'L' && h[3] == 'F' && h[5] == 1) {
        unsigned machine = h[18] | (static_cast<unsigned>(h[19]) << 8);
        if (machine == 3 || machine == 62) return FileClass::Exec;
        return FileClass::Normal;
    }
    if (got >= 2 && h[0] == 'M' && h[1] == 'Z') return FileClass::Exec;  // PE (Windows)
    if (exec_extensions().count(ext)) return FileClass::Exec;
    return FileClass::Normal;
}

}  // namespace zzipp
