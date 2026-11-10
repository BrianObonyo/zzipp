# The `.mzg` archive format (version 1)

All integers are **little-endian**. Offsets are absolute byte offsets from the start of the file.

```
+----------------------+  offset 0
| Header (32 bytes)    |
+----------------------+  offset 32
| Block 0              |  compressed (or stored) bytes
| Block 1              |
| ...                  |
+----------------------+  header.index_offset
| Index region         |  runs to the end of the file
+----------------------+
```

The header is written last (after the index offset is known), so a crashed or interrupted run
never leaves something that looks valid: `create` writes to `<name>.partial` and renames it
into place only after the header is finalised.

## Header (32 bytes)

| Offset | Size | Field          | Notes                                           |
|-------:|-----:|----------------|-------------------------------------------------|
| 0      | 4    | magic          | ASCII `MZG1`                                    |
| 4      | 2    | version        | `1`. Readers reject any other value.            |
| 6      | 2    | flags          | Reserved, `0`                                   |
| 8      | 8    | index_offset   | Start of the index region                       |
| 16     | 8    | index_size     | Must equal `file_size - index_offset`           |
| 24     | 4    | index_crc32    | CRC-32 of the whole index region (as stored)    |
| 28     | 4    | reserved       | `0`                                             |

## Blocks

A **block** is a run of file contents concatenated back to back (a *solid* block). Files are
grouped by type and extension before packing, so similar data sits together and the compressor's
dictionary can find matches across file boundaries.

Each block is stored as one of:

| Method | Value | Payload                                                        |
|-------:|------:|----------------------------------------------------------------|
| Store  | 0     | The raw bytes (used for already-compressed or incompressible data) |
| LZMA2  | 1     | A raw LZMA2 stream (no `.xz` container, no extra framing)      |
| BCJ+LZMA2 | 2  | x86 BCJ filter applied first, then a raw LZMA2 stream (for x86/x86-64 executables) |

To decode a raw LZMA2 stream you only need the dictionary size, which the index records
per block. Blocks are fully independent, which is what makes parallel compression,
parallel extraction and single-file extraction possible.

## Index region

```
u8   encoding      0 = stored, 1 = LZMA2
u64  raw_size      size of the serialised index after decoding (max 1 GiB)
u32  dict_size     LZMA2 dictionary size (0 when stored)
...  payload       the serialised index, stored or LZMA2-compressed
```

The serialised index is:

```
u32  block_count
block_count x Block record:
    u64 offset        absolute file offset of the block's bytes
    u64 comp_size     bytes on disk
    u64 raw_size      bytes after decoding (1 .. 4 GiB)
    u32 comp_crc32    CRC-32 of the bytes on disk
    u32 dict_size     LZMA2 dictionary (4 KiB .. 1.5 GiB), 0 for Store
    u8  method        0, 1 or 2

u32  entry_count
entry_count x Entry record:
    u8  kind          0 = file, 1 = directory
    u32 path_len + path bytes   UTF-8, '/'-separated, relative
    u64 size
    i64 mtime         seconds since the Unix epoch
    u32 mode          permission bits (& 0777)
    u32 crc32         CRC-32 of the file contents
    u32 block         index into the block table, or 0xFFFFFFFF for empty files / directories
    u64 offset        position of the file inside the *decoded* block
```

### Deduplication

Identical files are stored once. Their entries simply point at the same `(block, offset)`.
There is no separate "duplicate" record, so readers need no special handling.

### Validation rules

A conforming reader must reject (not crash on) an archive when any of these hold. The reference
implementation checks all of them when the archive is opened, before any data is decoded:

- bad magic, unsupported version, or `index_size != file_size - index_offset`
- index checksum mismatch, or an index that does not decode to exactly `raw_size` bytes
- a block that lies outside `[32, index_offset)`
- an unknown method, a zero or absurdly large block, a stored block with `comp_size != raw_size`,
  or an LZMA2 dictionary outside 4 KiB .. 1.5 GiB
- a path that is empty, absolute, contains a `.` or `..` component, an empty component, a
  backslash, or a NUL byte (on Windows also `:`)
- two entries with the same path
- a directory with data, a non-empty file with no block, or a file whose
  `offset + size` exceeds its block's `raw_size`
- trailing bytes after the index

Per-block CRCs are checked before decoding; per-file CRCs after.

## Design notes

- **Index at the end** lets the writer stream blocks out as they finish compressing without
  knowing the final layout up front, and lets `list` read only the tail of the file.
- **Per-block dictionary size** is stored, and capped to the block size at compression time,
  so a tiny block never forces the decoder to allocate a 64 MiB window.
- **No ZIP64-style complexity**: all sizes are 64-bit from the start.

## Possible future versions

Encryption (AES-256-GCM per block with an Argon2id-derived key), recovery records, split volumes,
and a streaming layout for files larger than a block. Any of these would use the `flags` field
and bump `version`.
