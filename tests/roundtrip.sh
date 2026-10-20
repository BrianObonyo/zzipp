#!/usr/bin/env bash
# End-to-end tests: create/list/test/extract round trip, dedup, overwrite protection,
# ZIP export, and corruption handling. Usage: roundtrip.sh /path/to/zzipp
set -euo pipefail

MZ="$(cd "$(dirname "${1:?usage: roundtrip.sh /path/to/zzipp}")" && pwd)/$(basename "$1")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

fail() { echo "FAIL: $*" >&2; exit 1; }

# ---------- sample tree ----------
mkdir -p src/sub/deep src/empty_dir "src/dir with spaces"
for i in $(seq 1 40); do seq 1 $((i * 200)) > "src/sub/file$i.txt"; done
head -c 300000 /dev/urandom > src/random.bin
cp src/random.bin src/sub/random_copy.bin              # exact duplicate
printf '\x89PNG\r\n\x1a\n' > src/fake.png               # "already compressed" by magic
head -c 50000 /dev/urandom >> src/fake.png
: > src/empty.txt
echo "hello" > "src/dir with spaces/hello world.txt"
echo "deep" > src/sub/deep/file.txt
cp /bin/ls src/ls_copy && chmod 755 src/ls_copy         # ELF executable -> BCJ path

# ---------- create / list / test / extract ----------
out=$("$MZ" create a.mzg src) || fail "create"
echo "$out"
echo "$out" | grep -q "1 duplicate" || fail "dedup did not detect the duplicate"
"$MZ" list a.mzg | grep -qx "src/sub/file7.txt" || fail "list missing file"
"$MZ" list -l a.mzg | grep -q "hello world.txt" || fail "list -l missing file"
"$MZ" test a.mzg || fail "test"
"$MZ" extract -o out a.mzg >/dev/null || fail "extract"
diff -r src out/src || fail "extracted tree differs from original"
[ -x out/src/ls_copy ] || fail "executable bit lost"
[ -d out/src/empty_dir ] || fail "empty directory lost"
[ -f out/src/empty.txt ] && [ ! -s out/src/empty.txt ] || fail "empty file wrong"

# ---------- presets and single-thread ----------
for flags in "--fast" "--max" "--level 0" "--no-dedup --no-sort -j 1" "--block 100k -j 4 --dict 64k"; do
  rm -f b.mzg
  # shellcheck disable=SC2086
  "$MZ" create $flags b.mzg src >/dev/null || fail "create $flags"
  "$MZ" test b.mzg >/dev/null || fail "test after create $flags"
  rm -rf out_b; "$MZ" extract -o out_b b.mzg >/dev/null; diff -r src out_b/src || fail "roundtrip with $flags"
done

# ---------- partial extraction ----------
"$MZ" extract -o part a.mzg src/sub/file7.txt "src/dir with spaces" >/dev/null || fail "partial extract"
cmp src/sub/file7.txt part/src/sub/file7.txt || fail "partial file differs"
[ -f "part/src/dir with spaces/hello world.txt" ] || fail "partial directory extract"
[ ! -e part/src/sub/file8.txt ] || fail "extracted unrequested file"
if "$MZ" extract -o part2 a.mzg no/such/path 2>/dev/null; then fail "missing path should fail"; fi

# ---------- safety behaviour ----------
if "$MZ" create a.mzg src 2>/dev/null; then fail "create must not overwrite without -f"; fi
"$MZ" create -f a.mzg src >/dev/null || fail "create -f"
if "$MZ" extract -o out a.mzg >/dev/null 2>&1; then fail "extract must not overwrite without -f"; fi
"$MZ" extract -f -o out a.mzg >/dev/null || fail "extract -f"

# archive created inside the directory being archived must not include itself
mkdir selfref && echo x > selfref/f.txt
( cd selfref && "$MZ" create inside.mzg . >/dev/null )
"$MZ" list selfref/inside.mzg | grep -q "inside.mzg" && fail "archive archived itself"

# ---------- ZIP export ----------
"$MZ" export-zip a.mzg a.zip >/dev/null || fail "export-zip"
if command -v unzip >/dev/null 2>&1; then
  unzip -tq a.zip >/dev/null || fail "unzip -t rejected the ZIP"
  mkdir zipout && unzip -q a.zip -d zipout
  diff -r src zipout/src || fail "ZIP contents differ"
elif command -v python3 >/dev/null 2>&1; then
  python3 -c "import zipfile,sys; sys.exit(1 if zipfile.ZipFile('a.zip').testzip() else 0)" || fail "python zipfile rejected the ZIP"
else
  echo "note: no unzip/python3 available, ZIP content not verified"
fi

# ---------- corruption: must fail cleanly (exit 1), never crash ----------
size=$(wc -c < a.mzg)
for n in 0 1 10 31 32 33 $((size / 2)) $((size - 1)); do
  head -c "$n" a.mzg > trunc.mzg
  set +e; "$MZ" test trunc.mzg >/dev/null 2>&1; rc=$?; set -e
  [ "$rc" -eq 1 ] || fail "truncated archive ($n bytes) gave exit code $rc, expected 1"
done

for i in $(seq 1 150); do
  cp a.mzg bad.mzg
  off=$(( (RANDOM * 32768 + RANDOM) % size ))
  printf "\\x$(printf '%02x' $((RANDOM % 256)))" | dd of=bad.mzg bs=1 seek="$off" conv=notrunc 2>/dev/null
  set +e; "$MZ" test bad.mzg >/dev/null 2>&1; rc=$?; set -e
  [ "$rc" -le 1 ] || fail "corrupt archive (byte $off) crashed with exit code $rc"
done

echo "roundtrip: all checks passed"
