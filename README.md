# zzipp

zzipp is a solid-compression archiver that creates `.mzg` archives using LZMA2. It includes a command-line tool and an optional Qt 6 desktop app. Archives can also be exported to standard ZIP files.

## Features

- Create, list, test, and extract `.mzg` archives.
- Compress files in solid blocks, with fast, normal, and maximum presets.
- Deduplicate identical files and group similar files by extension.
- Extract selected paths and process blocks in parallel.
- Export `.mzg` archives to ZIP.
- Browse and extract archives with the optional desktop app.

## Requirements

- CMake 3.16 or newer
- A C++17 compiler
- liblzma and zlib development libraries
- Bash to configure and run the shell-based tests
- Qt 6.2 or newer for the optional desktop app

On Debian or Ubuntu, install the dependencies with:

```sh
sudo apt install build-essential cmake liblzma-dev zlib1g-dev bash
```

Add `qt6-base-dev` to build the desktop app.

## Build

```sh
cmake -S . -B build
cmake --build build --parallel
```

If Qt 6 is available, CMake also builds the desktop app. To build only the command-line tool:

```sh
cmake -S . -B build -DZZIPP_BUILD_GUI=OFF
cmake --build build --parallel
```

The CLI executable is `build/zzipp` on Unix-like systems. On multi-configuration generators such as Visual Studio, it may be under a configuration directory (for example, `build/Release/zzipp.exe`).

## Command-line usage

```text
zzipp create [options] <archive.mzg> <file|dir>...
zzipp extract [-o DIR] [-f] [-j N] <archive.mzg> [path...]
zzipp list [-l] <archive.mzg>
zzipp info <archive.mzg>
zzipp test [-j N] <archive.mzg>
zzipp export-zip [-f] [--level N] <archive.mzg> <out.zip>
```

Examples:

```sh
# Create an archive from a folder
zzipp create backup.mzg my-folder

# Inspect, list, and verify the archive
zzipp info backup.mzg
zzipp list -l backup.mzg
zzipp test backup.mzg

# Extract everything, or just a selected path
zzipp extract -o restored backup.mzg
zzipp extract -o restored backup.mzg my-folder/report.txt

# Export it as a ZIP file
zzipp export-zip backup.mzg backup.zip
```

Creation options include `--fast`, `--normal`, `--max`, `--level N` (0-9), `--dict SIZE`, `--block SIZE`, `-j N`, `--no-dedup`, `--no-sort`, `-f` to overwrite, and `-v` for per-block progress. Use `zzipp --help` for the complete usage text.

## Desktop app

When Qt 6.2 or newer is found, the build includes `zzipp-gui`. It can create archives, browse and extract them, verify integrity, and export ZIP files. See [docs/GUI.md](docs/GUI.md) for screenshots, details, and GUI-specific build information.

## Tests

```sh
ctest --test-dir build --output-on-failure
```

The suite includes unit tests and command-line round-trip and edge-case tests. GUI tests are included when the GUI is built.

## Archive format

The `.mzg` format stores a validated index at the end of the archive and supports stored, LZMA2, and BCJ+LZMA2 blocks. See [docs/FORMAT.md](docs/FORMAT.md) for the version 1 format specification.

## License

zzipp is licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
