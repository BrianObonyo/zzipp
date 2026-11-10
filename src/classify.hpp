// Decides how a file should be treated: compressed normally, run through the x86
// BCJ filter (executables), or stored as-is (data that is already compressed).
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace zzipp {

enum class FileClass { Normal, Exec, Store };

FileClass classify_file(const std::filesystem::path& path);
std::string lowercase_extension(const std::filesystem::path& path);  // without the dot

}  // namespace zzipp
