# zzipp

A fast, solid-compression archiverr in C++17 with LZMA2, deduplication, multithreading, and ZIP export.

# zzipp

A fast C++17 archiver featuring solid LZMA2 compression, deduplication, multithreading, and ZIP export.

**zzipp** packs files and folders into a single `.mzg` archive. It compresses multiple files in solid blocks to reduce redundant data and improve compression efficiency. It also supports exporting archives to standard `.zip` files.

```bash
zzipp create project.mzg my-project/ --max -j 4
zzipp list -l project.mzg
zzipp extract -o restored project.mzg
zzipp export-zip project.mzg project.zip
```

Example output from a benchmark run:

```text
project.mzg: 5,871 files, 751 dirs (2248 duplicates stored once)
  input : 84.5 MiB
  output: 4.2 MiB (5.0% of input)
  blocks: 1 lzma2, 0 bcj+lzma2, 0 stored
```

The figures above are illustrative results from a particular dataset, not a guarantee of compression ratios on other files.

## Features

- **Solid compression:** Groups files into blocks and compresses them together using LZMA2 through liblzma.
- **Smart layout:** Sorts files by type and extension so similar data is stored close together.
- **Executable filtering:** Applies the x86/x86-64 BCJ filter to supported ELF and PE binaries before LZMA2 compression.
- **Already-compressed file detection:** Uses file signatures and extensions to identify formats such as PNG, JPEG, MP4, and ZIP that generally do not benefit from additional compression.
- **Deduplication:** Stores identical files once, using size checks, hashing, and byte-for-byte confirmation.
- **Multithreading:** Compresses blocks in parallel while writing them in order. Extraction and archive testing also support parallel execution.
- **Random access:** Extracts individual files or folders by decoding only the blocks they require.
- **Integrity checks:** Uses CRC-32 for blocks, files, and the index. The `test` command verifies archive integrity.
- **ZIP export:** Exports archives to conventional ZIP files using DEFLATE compression, compatible with common operating systems and archive tools.
- **Hardened reader:** Validates untrusted archive fields before use. See [Security](#security).
- **Crash-safe writes:** Writes output to a `.partial` file and renames it only after successful completion.

## Build

### Requirements

- A C++17-compatible compiler
- CMake 3.16 or newer
- liblzma
- zlib

### Debian / Ubuntu

```bash
sudo apt update
sudo apt install build-essential cmake liblzma-dev zlib1g-dev
```

### macOS

```bash
brew install cmake xz
```

### Compile and test

Run these commands from the project root:

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Optional installation:

```bash
sudo cmake --install build
```

**Platform status:** Developed and tested on Linux (Ubuntu 24.04, GCC 13). The CI workflow also targets macOS. Windows is currently untested.

## Usage

```text
zzipp create      [options] <archive.mzg> <file|dir>...
zzipp extract     [-o DIR] [-f] [-j N] <archive.mzg> [path...]
zzipp list        [-l] <archive.mzg>
zzipp test         [-j N] <archive.mzg>
zzipp export-zip  [-f] [--level N] <archive.mzg> <out.zip>
```

### Create options

| Option | Meaning |
|---|---|
| `--fast` / `--normal` / `--max` | Speed-versus-compression-ratio preset. Default: `--normal`. |
| `--level N` | LZMA preset from 0 to 9; overrides the preset. |
| `--dict SIZE` | Dictionary size, such as `64m`. Default depends on the preset. |
| `--block SIZE` | Solid block size, such as `256m`. Default: `32m`. Larger blocks can improve compression but increase memory use and reduce parallelism. |
| `-j N` | Number of worker threads. Default: CPU count, capped at 8. |
| `--no-dedup` | Disable deduplication. |
| `--no-sort` | Disable extension-based grouping. |
| `-f` | Overwrite an existing archive. For extraction, overwrite existing files. |
| `-v` | Show per-block progress. |

Without `-f`, zzipp refuses to overwrite existing output.

Extraction can target individual files or directories:

```bash
zzipp extract -o restored project.mzg src/docs src/readme.txt
```

### Compression tip

For a potentially better compression ratio, try:

```bash
zzipp create project.mzg my-project/ --max --block 256m --dict 64m
```

Large dictionaries and blocks can require several hundred MiB of RAM per worker, depending on the workload and settings. Measure memory use before using these settings on large datasets.

## How it works

```text
inputs -> scan -> dedup -> classify -> sort -> pack into blocks
                                                   |
                         +-------------------------+-------------------------+
                         v                         v                         v
                     worker 1                  worker 2        ...       worker N
              read -> CRC -> LZMA2 | BCJ+LZMA2 | store
                         +-------------------------+-------------------------+
                                                   v
                                  writer (in block order) -> index -> header
```

1. **Scan:** Walks input paths using `std::filesystem`. Symlinks and special files are skipped with a warning.
2. **Deduplicate:** Files of different sizes cannot be identical. Same-size files are hashed using CRC-64 and then confirmed byte-for-byte. Duplicate entries reference the same stored data.
3. **Classify and sort:** Groups normal files, executables, and already-compressed files, then sorts by extension and path.
4. **Pack:** Concatenates file data into blocks up to the configured block size. A file larger than the block size receives its own block.
5. **Compress:** Each block is encoded independently as a raw LZMA2 stream, optionally using the BCJ filter. If compression does not reduce the size, the block is stored uncompressed.
6. **Index:** Writes paths, sizes, modification times, modes, CRCs, and block locations at the end of the archive. The index is itself LZMA2-compressed.

The on-disk layout is documented in [`docs/FORMAT.md`](docs/FORMAT.md).

## Design decisions

- **liblzma instead of a custom codec:** Building a competitive LZMA encoder is a substantial undertaking. The engineering focus is on the archive container, storage layout, concurrency, and robustness.
- **Independent blocks:** Sacrifices some compression efficiency at block boundaries in exchange for parallel processing and single-file extraction.
- **Raw LZMA2 instead of `.xz`:** Avoids per-block container overhead. Dictionary parameters are recorded in the archive metadata.
- **Index at the end:** Allows the writer to emit blocks as they finish. Listing can locate the index from the archive tail.

## Benchmarks

Measurements were taken in a single-core Linux container, using one thread for each tool. Sizes are exact bytes; percentages represent the proportion of input size.

Reproduce the measurements using:

```bash
scripts/bench.sh build/zzipp <dir> [threads]
```

The script uses xz, ZIP, 7-Zip, and RAR if available.

### Dataset 1: 88 MB of C/C++ system headers

5,871 files with many duplicates.

| Tool / settings | Size (bytes) | % of input | Time |
|---|---:|---:|---:|
| zzipp `--fast` | 5,551,426 | 6.3% | 1.5 s |
| zzipp `--normal` | 4,541,767 | 5.1% | 12.1 s |
| zzipp `--max` | 4,455,698 | 5.0% | 17.6 s |
| zzipp `--max --dict 64m --block 256m` | 4,438,117 | 5.0% | 17.6 s |
| tar + xz `-9e` | 4,501,896 | 5.1% | 31.7 s |
| 7-Zip `-mx=9` (solid) | 4,490,263 | 5.1% | 25.5 s |
| zzipp ZIP export | 19,492,790 | 22.0% | 19.2 s |
| zip `-9` | 19,804,662 | 22.4% | 2.8 s |

### Dataset 2: 65 MB of x86-64 executables

293 files from `/usr/bin`.

| Tool / settings | Size (bytes) | % of input | Time |
|---|---:|---:|---:|
| zzipp `--fast` | 20,131,083 | 31.0% | 4.3 s |
| zzipp `--normal` | 17,356,156 | 26.8% | 30.6 s |
| zzipp `--max` | 15,555,493 | 24.0% | 38.2 s |
| zzipp `--max --dict 64m --block 256m` | 13,462,797 | 20.8% | 44.9 s |
| tar + xz `-9e` | 14,396,592 | 22.2% | 48.6 s |
| 7-Zip `-mx=9` (solid) | 12,267,726 | 18.9% | 33.3 s |
| zzipp ZIP export | 27,488,137 | 42.4% | 49.4 s |
| zip `-9` | 27,379,079 | 42.2% | 8.1 s |

### Interpreting the results

- On text-heavy input, zzipp is competitive with xz and 7-Zip in these measurements. Deduplication contributes to the result because the dataset contains many identical files.
- On executables, zzipp outperforms xz in these measurements but trails 7-Zip. Increasing the block size improves the measured ratio from 24.0% to 20.8%.
- ZIP export produces sizes comparable to conventional ZIP compression on these datasets. ZIP normally compresses files independently, so it cannot share a solid compression dictionary across files.
- These measurements cover only two datasets on one machine. Benchmark your own workloads before drawing broader conclusions.
- RAR was not benchmarked because it is proprietary and was not installed in the test environment.

## Security

Archives are treated as untrusted input.

- **Path validation:** Rejects absolute paths, `..`, empty components, backslashes, and NUL bytes to prevent extraction outside the target directory (zip-slip).
- **Bounds checking:** Validates offsets, sizes, counts, and dictionary sizes before decoding archive data.
- **Symlink protection:** Extraction neither follows nor creates symlinks and refuses to overwrite one.
- **Parser robustness:** The test suite mutates and truncates archives thousands of times and expects clean errors instead of crashes. CI runs AddressSanitizer and UBSan.

### Known security limitation

A block can declare up to 4 GiB of decoded data, potentially causing a reader to allocate a large amount of memory for a malicious archive. This remains a known limitation.

## Limitations and roadmap

### Current limitations

- Each block, including any individual file larger than `--block`, is loaded fully into memory, with a 4 GiB cap per block. A streaming path for very large files is planned.
- Symlinks, hard links, ownership, and extended attributes are not preserved.
- Timestamps have one-second resolution.
- ZIP export supports classic ZIP only: up to 65,535 entries and 4 GiB per file or archive.
- The BCJ filter supports x86/x86-64 only; ARM filters are not yet available.
- Encryption, recovery records, and split volumes are not implemented.

### Planned improvements

- AES-256-GCM encryption with Argon2id key derivation
- Multi-volume archives
- ARM and delta filters
- Adaptive block splitting
- Streaming support for very large files

These are planned features, not current capabilities.

## Testing

Run the test suite:

```bash
ctest --test-dir build --output-on-failure
```

Build with sanitizers and test:

```bash
cmake -S . -B build-asan -Dzzipp_SANITIZE=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

The test files cover:

- `tests/unit_tests.cpp`: byte I/O, size parsing, path safety, codec round trips, index validation, and parser mutation testing.
- `tests/roundtrip.sh`: archive creation, listing, testing, extraction, recursive comparison, deduplication, overwrite protection, ZIP validation using `unzip`, truncated archives, and bit-flipped archives.

## Project layout

| Path | Purpose |
|---|---|
| `src/main.cpp` | Command-line interface parsing |
| `src/create.cpp` | Scanning, deduplication, block planning, and parallel compression |
| `src/extract.cpp` | Extraction, listing, and archive testing |
| `src/zip_export.cpp` | ZIP writer using zlib DEFLATE |
| `src/archive.cpp` | Archive reader and index region |
| `src/format.cpp` | On-disk structures and validation |
| `src/codec.cpp` | Raw LZMA2 and BCJ wrappers using liblzma |
| `src/classify.cpp` | File-type detection |
| `docs/FORMAT.md` | Archive format specification |
| `scripts/bench.sh` | Benchmark script |

## License

zzipp is licensed under the **Apache License 2.0**.

Copyright 2026 Brian Obonyo.

See [`LICENSE`](LICENSE) for the complete license text.
