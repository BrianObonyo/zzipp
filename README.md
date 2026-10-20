# zzipp

**A fast, solid-compression archiver in C++17 with LZMA2, deduplication, multithreading, and ZIP export.**

*zzipp* is Swahili for "load" or "cargo". It packs files and folders into a single `.mzg` archive,
the way 7-Zip and RAR do: many files compressed together as one stream so repeated content across
files is found and removed. It can also export any archive to a standard `.zip`.

```console
$ zzipp create project.mzg my-project/ --max -j 4
project.mzg: 5,871 files, 751 dirs (2248 duplicates stored once)
  input : 84.5 MiB
  output: 4.2 MiB (5.0% of input)
  blocks: 1 lzma2, 0 bcj+lzma2, 0 stored
$ zzipp list -l project.mzg
$ zzipp extract -o restored project.mzg
$ zzipp export-zip project.mzg project.zip     # opens anywhere
```

## Features

- **Solid compression**: files are grouped into blocks and compressed together (LZMA2 via liblzma).
- **Smart layout**: files are sorted by type and extension so similar data sits next to each other.
- **Executable filter**: x86/x86-64 binaries (ELF, PE) go through the BCJ filter before LZMA2.
- **Skips the pointless**: PNG/JPEG/MP4/ZIP/etc. (detected by magic bytes and extension) are stored as-is. Anything that does not shrink is stored too.
- **Deduplication**: identical files are stored once (size, then hash, then byte-for-byte confirmation).
- **Multithreaded**: blocks are compressed in parallel and written in order. Extraction and `test` are parallel too.
- **Random access**: extract one file or folder; only the blocks that contain it are decoded.
- **Integrity**: CRC-32 on every block, every file, and the index. `zzipp test` verifies everything.
- **ZIP export**: `export-zip` writes a classic ZIP (deflate) that Windows, macOS, and Linux open natively.
- **Hardened reader**: every field of an untrusted archive is validated before use (see [Security](#security)).
- **Crash-safe writes**: output goes to `<name>.partial` and is renamed only when complete.

## Build

Requirements: a C++17 compiler, CMake 3.16+, liblzma, zlib.

```bash
# Debian/Ubuntu
sudo apt install build-essential cmake liblzma-dev zlib1g-dev
# macOS
brew install cmake xz

cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
sudo cmake --install build        # optional
```

Developed and tested on Linux (Ubuntu 24.04, GCC 13). The CI workflow also targets macOS; Windows is untested.

## Usage

```text
zzipp create  [options] <archive.mzg> <file|dir>...
zzipp extract [-o DIR] [-f] [-j N] <archive.mzg> [path...]
zzipp list    [-l] <archive.mzg>
zzipp test    [-j N] <archive.mzg>
zzipp export-zip [-f] [--level N] <archive.mzg> <out.zip>
```

| `create` option | Meaning |
|---|---|
| `--fast` / `--normal` / `--max` | Speed vs. ratio preset (default `--normal`). |
| `--level N` | LZMA preset 0 to 9 (overrides the above). |
| `--dict SIZE` | Dictionary size, e.g. `64m`. Default comes from the preset. |
| `--block SIZE` | Solid block size, e.g. `256m` (default `32m`). Bigger blocks compress better but use more memory and parallelise less. |
| `-j N` | Threads (default: CPU count, max 8). |
| `--no-dedup`, `--no-sort` | Turn off deduplication or extension grouping. |
| `-f` | Overwrite an existing archive. `extract -f` overwrites existing files. |
| `-v` | Per-block progress. |

Without `-f`, zzipp refuses to overwrite anything. `extract` accepts file or directory paths to pull out
a subset: `zzipp extract a.mzg src/docs src/readme.txt`.

**Tip:** for the best ratio use `--max --block 256m --dict 64m` (needs several hundred MiB of RAM per thread).

## How it works

```
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

1. **Scan**: walk inputs with `std::filesystem`; symlinks and special files are skipped with a warning.
2. **Dedup**: files with a unique size cannot be duplicates, so only same-size files are hashed (CRC-64) and confirmed byte-for-byte. Duplicates become index entries pointing at the same bytes.
3. **Classify and sort**: normal / executable / already-compressed, then by extension and path.
4. **Pack**: files are concatenated into blocks up to `--block` bytes (a larger file gets its own block).
5. **Compress**: each block is encoded independently as a raw LZMA2 stream. If the result is not smaller, the block is stored.
6. **Index**: paths, sizes, mtimes, modes, CRCs and block locations are written at the end of the file, themselves LZMA2-compressed.

The byte-level layout is in [docs/FORMAT.md](docs/FORMAT.md).

### Design decisions

- **liblzma instead of a hand-written codec.** A competitive LZMA encoder is months of work. The portfolio value is in the archiver around it: container format, layout heuristics, concurrency, and robustness.
- **Independent blocks.** Costs a little ratio at block boundaries, buys parallelism and single-file extraction.
- **Raw LZMA2, not `.xz`.** No container overhead per block; the index records the dictionary size.
- **Index at the end.** The writer can stream blocks as they finish; `list` only reads the tail.

## Benchmarks

Measured on a single-core Linux container, one thread for every tool. Sizes are exact bytes; percentages are of input size.
Reproduce with `scripts/bench.sh build/zzipp <dir> [threads]` (uses xz, zip, 7z and rar if installed).

**88 MB of C/C++ system headers (5,871 files, many duplicates)**

| Tool / settings | Size | % of input | Time |
|---|---:|---:|---:|
| zzipp `--fast` | 5,551,426 | 6.3% | 1.5 s |
| zzipp `--normal` | 4,541,767 | 5.1% | 12.1 s |
| zzipp `--max` | 4,455,698 | 5.0% | 17.6 s |
| zzipp `--max --dict 64m --block 256m` | 4,438,117 | 5.0% | 17.6 s |
| tar + xz -9e | 4,501,896 | 5.1% | 31.7 s |
| 7z -mx=9 (solid) | 4,490,263 | 5.1% | 25.5 s |
| zzipp `export-zip` | 19,492,790 | 22.0% | 19.2 s |
| zip -9 | 19,804,662 | 22.4% | 2.8 s |

**65 MB of x86-64 executables (293 files from `/usr/bin`)**

| Tool / settings | Size | % of input | Time |
|---|---:|---:|---:|
| zzipp `--fast` | 20,131,083 | 31.0% | 4.3 s |
| zzipp `--normal` | 17,356,156 | 26.8% | 30.6 s |
| zzipp `--max` | 15,555,493 | 24.0% | 38.2 s |
| zzipp `--max --dict 64m --block 256m` | 13,462,797 | 20.8% | 44.9 s |
| tar + xz -9e | 14,396,592 | 22.2% | 48.6 s |
| 7z -mx=9 (solid) | 12,267,726 | 18.9% | 33.3 s |
| zzipp `export-zip` | 27,488,137 | 42.4% | 49.4 s |
| zip -9 | 27,379,079 | 42.2% | 8.1 s |

Reading the numbers honestly:

- On text, zzipp matches or slightly beats xz and 7-Zip. Part of that is deduplication, since this dataset has many identical files.
- On executables, zzipp beats `xz` but trails 7-Zip by about 2 points. Block size matters a lot here (24.0% to 20.8%), which is why large blocks are recommended for `--max`.
- ZIP export is as big as `zip -9`, by design: ZIP compresses each file separately and cannot share a dictionary.
- These are two datasets on one machine. Run the script on your own data before drawing conclusions.
- Not benchmarked: RAR (proprietary, not installed).

## Security

Archives are treated as untrusted input.

- All paths are validated: no absolute paths, `..`, empty components, backslashes, or NUL bytes, so a crafted archive cannot write outside the extraction folder ("zip-slip").
- Every offset, size, count and dictionary size is range-checked when the archive opens, before any data is decoded.
- Extraction never follows or creates symlinks and refuses to overwrite one.
- The test suite mutates and truncates archives thousands of times and requires a clean error, never a crash. It runs under AddressSanitizer and UBSan in CI.
- Known gap: a block may declare up to 4 GiB of decoded size, so a maliciously crafted archive can make a reader allocate a lot of memory.

## Limitations and roadmap

- Each block, and so any single file larger than `--block`, is loaded fully into memory (4 GiB cap per block). A streaming path for huge files is planned.
- Not preserved: symlinks, hard links, ownership, extended attributes. Timestamps have 1 second resolution.
- ZIP export is classic ZIP only: at most 65,535 entries and 4 GiB per file/archive.
- The BCJ filter targets x86/x86-64 only (no ARM filters yet).
- No encryption, recovery records, or split volumes yet. Planned: AES-256-GCM with Argon2id, multi-volume archives, ARM/delta filters, adaptive block splitting.

## Testing

```bash
ctest --test-dir build --output-on-failure           # unit + end-to-end
cmake -S . -B build-asan -Dzzipp_SANITIZE=ON && cmake --build build-asan -j && ctest --test-dir build-asan
```

- `tests/unit_tests.cpp`: byte I/O, size parsing, path safety, codec round trips, index validation, mutation testing of the parser.
- `tests/roundtrip.sh`: create, list, test, extract and `diff -r`; dedup; overwrite protection; ZIP validation with `unzip`; truncated and bit-flipped archives.

## Project layout

| Path | Purpose |
|---|---|
| `src/main.cpp` | CLI parsing |
| `src/create.cpp` | Scan, dedup, block planning, parallel compression pipeline |
| `src/extract.cpp` | `extract`, `list`, `test` |
| `src/zip_export.cpp` | ZIP writer (zlib deflate) |
| `src/archive.cpp` | Archive reader and index region |
| `src/format.cpp` | On-disk structures, validation |
| `src/codec.cpp` | liblzma raw LZMA2/BCJ wrappers |
| `src/classify.cpp` | File-type detection |
| `docs/FORMAT.md` | Format specification |
| `scripts/bench.sh` | Benchmark script |

## License

MIT. See [LICENSE](LICENSE).
