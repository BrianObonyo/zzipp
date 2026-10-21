#!/usr/bin/env bash
# Edge cases, error messages and exit codes. Usage: edge_cases.sh /path/to/zzipp
# Exit codes: 0 ok, 1 runtime error, 2 usage error. Anything >= 3 means a crash or sanitizer report.
set -uo pipefail
ZZ="$(cd "$(dirname "${1:?usage: edge_cases.sh /path/to/zzipp}")" && pwd)/$(basename "$1")"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT; cd "$WORK"
export ASAN_OPTIONS=exitcode=99:detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:exitcode=98
FAILS=0
fail() { echo "FAIL: $*" >&2; FAILS=$((FAILS + 1)); }

# expect <exit code> <stderr/stdout substring or ""> -- command...
expect() {
  local want=$1 text=$2; shift 3
  local out rc; out=$("$@" 2>&1); rc=$?
  [ "$rc" -eq "$want" ] || fail "exit $rc, wanted $want: $* -> $out"
  if [ -n "$text" ] && ! grep -qi -- "$text" <<<"$out"; then fail "message lacks '$text': $* -> $out"; fi
}

# ---------- usage errors ----------
expect 2 "usage" -- "$ZZ"
expect 2 "unknown command" -- "$ZZ" frobnicate
expect 2 "unknown option" -- "$ZZ" create --bogus a.mzg x
expect 2 "needs an archive name" -- "$ZZ" create a.mzg
expect 2 "invalid size" -- "$ZZ" create --dict abc a.mzg x
expect 2 "needs a value" -- "$ZZ" create a.mzg x --block
expect 2 "invalid thread count" -- "$ZZ" create -j 0 a.mzg x
expect 2 "must be 0-9" -- "$ZZ" create --level 12 a.mzg x
expect 0 "zzipp" -- "$ZZ" --version

# ---------- missing / wrong inputs ----------
expect 1 "cannot access" -- "$ZZ" create a.mzg /no/such/file
expect 1 "cannot open archive" -- "$ZZ" list nothere.mzg
expect 1 "cannot open archive" -- "$ZZ" test nothere.mzg
expect 1 "cannot open archive" -- "$ZZ" extract nothere.mzg
echo "just text, definitely not an archive, but longer than thirty-two bytes" > text.mzg
expect 1 "not a zzipp archive" -- "$ZZ" list text.mzg
: > empty.mzg
expect 1 "too small" -- "$ZZ" test empty.mzg
mkdir adir
expect 1 "" -- "$ZZ" list adir
echo hi > f.txt
expect 1 "cannot create output" -- "$ZZ" create /no/such/dir/a.mzg f.txt
expect 1 "unsupported path" -- "$ZZ" create root.mzg /
expect 1 "duplicate path" -- "$ZZ" create dup.mzg f.txt f.txt
expect 1 "nothing to archive" -- "$ZZ" create none.mzg /dev/null

# ---------- archives with no data blocks ----------
mkdir -p onlydirs/a/b onlyempty
: > onlyempty/e1; : > onlyempty/e2
expect 0 "" -- "$ZZ" create d.mzg onlydirs
expect 0 "OK" -- "$ZZ" test d.mzg
expect 0 "" -- "$ZZ" extract -o d_out d.mzg
[ -d d_out/onlydirs/a/b ] || fail "empty directory tree not restored"
expect 0 "" -- "$ZZ" create e.mzg onlyempty
expect 0 "" -- "$ZZ" extract -o e_out e.mzg
[ -f e_out/onlyempty/e1 ] && [ ! -s e_out/onlyempty/e2 ] || fail "empty files not restored"
expect 0 "" -- "$ZZ" export-zip e.mzg e.zip
unzip -tq e.zip >/dev/null 2>&1 || fail "ZIP of empty files rejected"

# ---------- awkward names, relative paths, symlinks ----------
mkdir -p odd
printf 'x' > "odd/-leading-dash.txt"
printf 'x' > "odd/new
line.txt"
printf 'x' > "odd/$(printf 'a%.0s' $(seq 1 200)).txt"
printf 'x' > "odd/ünï çödé 日本語.txt"
ln -s f.txt odd/link
out=$("$ZZ" create odd.mzg ./odd/ 2>&1); [ $? -eq 0 ] || fail "odd names: $out"
grep -qi "symlink" <<<"$out" || fail "symlink skip was not reported: $out"
expect 0 "" -- "$ZZ" extract -o odd_out odd.mzg
diff <(cd odd && find . -type f | LC_ALL=C sort) <(cd odd_out/odd && find . -type f | LC_ALL=C sort) >/dev/null || fail "odd file names did not round-trip"
mkdir -p rel/sub && echo r > rel/sub/r.txt
expect 0 "" -- "$ZZ" create rel1.mzg rel/../rel/sub/../sub
"$ZZ" list rel1.mzg | grep -qx "sub/r.txt" || fail "relative path normalisation: $("$ZZ" list rel1.mzg)"

# ---------- dir/file selector boundaries ----------
mkdir -p sel/docs sel/docs2 && echo a > sel/docs/a && echo b > sel/docs2/b
"$ZZ" create sel.mzg sel >/dev/null
"$ZZ" extract -o sel_out sel.mzg sel/docs >/dev/null
[ -f sel_out/sel/docs/a ] && [ ! -e sel_out/sel/docs2 ] || fail "'sel/docs' must not select 'sel/docs2'"

# ---------- extraction targets and overwrite behaviour ----------
echo x > afile
expect 1 "" -- "$ZZ" extract -o afile sel.mzg
mkdir -p ro && echo data > ro/readonly.txt && chmod 444 ro/readonly.txt
"$ZZ" create ro.mzg ro >/dev/null
"$ZZ" extract -o ro_out ro.mzg >/dev/null
expect 1 "exists" -- "$ZZ" extract -o ro_out ro.mzg
expect 0 "" -- "$ZZ" extract -f -o ro_out ro.mzg     # overwriting a read-only file must work
[ "$(cat ro_out/ro/readonly.txt)" = "data" ] || fail "read-only file not overwritten correctly"
chmod -R u+w ro_out 2>/dev/null

# ---------- file larger than the block size, many threads ----------
head -c 5000000 /dev/urandom > big.bin
cp big.bin big2.bin; seq 1 400000 > big.txt
for j in 1 4; do
  rm -f big.mzg
  expect 0 "" -- "$ZZ" create -j $j --block 1m big.mzg big.bin big2.bin big.txt
  expect 0 "OK" -- "$ZZ" test -j $j big.mzg
  rm -rf big_out; "$ZZ" extract -j $j -o big_out big.mzg >/dev/null
  cmp big.bin big_out/big.bin && cmp big.txt big_out/big.txt && cmp big.bin big_out/big2.bin || fail "big file round-trip (-j $j)"
done

# ---------- timestamps and permissions survive ----------
if stat -c %Y f.txt >/dev/null 2>&1; then
  touch -d '2020-02-03 04:05:06' f.txt; chmod 640 f.txt
  "$ZZ" create meta.mzg f.txt >/dev/null; "$ZZ" extract -o meta_out meta.mzg >/dev/null
  [ "$(stat -c %Y f.txt)" = "$(stat -c %Y meta_out/f.txt)" ] || fail "mtime not preserved"
  [ "$(stat -c %a meta_out/f.txt)" = "640" ] || fail "permissions not preserved"
fi

# ---------- hostile archive: out-of-range mtime must not crash extraction ----------
python3 - <<'PY'
import struct, zlib, subprocess, sys, os
# Build a valid archive by hand with an absurd mtime, using the documented format.
data = b"hello"
crc = zlib.crc32(data) & 0xffffffff
def u8(v): return struct.pack('<B', v)
def u32(v): return struct.pack('<I', v)
def u64(v): return struct.pack('<Q', v)
def i64(v): return struct.pack('<q', v)
for name, mtime in (("evil_time.mzg", 2**62), ("evil_time2.mzg", -2**62)):
    block = data                                   # stored block
    idx = u32(1) + u64(32) + u64(len(block)) + u64(len(data)) + u32(zlib.crc32(block) & 0xffffffff) + u32(0) + u8(0)
    idx += u32(1) + u8(0) + u32(5) + b"f.txt" + u64(len(data)) + i64(mtime) + u32(0o644) + u32(crc) + u32(0) + u64(0)
    import lzma
    region = u8(0) + u64(len(idx)) + u32(0) + idx
    icrc = zlib.crc32(region) & 0xffffffff
    hdr = b"MZG1" + struct.pack('<HH', 1, 0) + u64(32 + len(block)) + u64(len(region)) + u32(icrc) + u32(0)
    open(name, 'wb').write(hdr + block + region)
PY
for a in evil_time.mzg evil_time2.mzg; do
  rm -rf evil_out; expect 0 "" -- "$ZZ" extract -o evil_out "$a"
  [ "$(cat evil_out/f.txt 2>/dev/null)" = "hello" ] || fail "hand-built archive $a did not extract"
  expect 0 "" -- "$ZZ" export-zip -f "$a" evil.zip
  expect 0 "" -- "$ZZ" list -l "$a"
done

# ---------- corruption fuzz across every command (sanitizer exit codes count as crashes) ----------
"$ZZ" create -f fz.mzg odd sel big.txt >/dev/null
size=$(wc -c < fz.mzg)
for i in $(seq 1 200); do
  cp fz.mzg bad.mzg
  for k in 1 2; do
    off=$(( (RANDOM * 32768 + RANDOM) % size ))
    printf "\\x$(printf '%02x' $((RANDOM % 256)))" | dd of=bad.mzg bs=1 seek="$off" conv=notrunc 2>/dev/null
  done
  for cmd in "list -l bad.mzg" "test bad.mzg" "extract -f -o fz_out bad.mzg" "export-zip -f bad.mzg fz.zip"; do
    # shellcheck disable=SC2086
    "$ZZ" $cmd >/dev/null 2>&1; rc=$?
    [ "$rc" -le 1 ] || { fail "corrupt archive crashed ('$cmd', rc=$rc, iteration $i)"; cp bad.mzg "$WORK/../zzipp-crash-$i.mzg" 2>/dev/null; break 2; }
  done
done
rm -rf fz_out

# ---------- error messages for damaged archives should say what is wrong ----------
cp fz.mzg trunc.mzg; truncate -s $((size - 10)) trunc.mzg
expect 1 "truncated or its index is damaged" -- "$ZZ" list trunc.mzg
cp fz.mzg flip.mzg; printf '\xff' | dd of=flip.mzg bs=1 seek=100 conv=notrunc 2>/dev/null
out=$("$ZZ" test flip.mzg 2>&1); rc=$?
[ "$rc" -eq 1 ] && grep -qi "corrupt\|mismatch\|checksum" <<<"$out" || fail "flipped data byte: rc=$rc msg=$out"

[ "$FAILS" -eq 0 ] && echo "edge cases: all checks passed" || { echo "$FAILS check(s) failed" >&2; exit 1; }
