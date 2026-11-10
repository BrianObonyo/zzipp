#!/usr/bin/env bash
# Compare zzipp against other archivers on a directory.
# Usage: scripts/bench.sh /path/to/zzipp /path/to/dataset [threads]
# Prints a Markdown table. Tools that are not installed are skipped.
set -uo pipefail
MZ="${1:?usage: bench.sh /path/to/zzipp /path/to/dataset [threads]}"
DATA="${2:?dataset directory required}"
J="${3:-1}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
PARENT="$(dirname "$(cd "$DATA" && pwd)")"; NAME="$(basename "$(cd "$DATA" && pwd)")"

now() { perl -MTime::HiRes=time -e 'printf "%.3f", time'; }
INPUT=$(du -sb "$DATA" | cut -f1)
echo "Dataset: $DATA ($INPUT bytes), threads for zzipp/xz/7z: $J"
echo
echo "| Tool / settings | Size (bytes) | % of input | Time (s) |"
echo "|---|---:|---:|---:|"

row() { # name, output file, start, end
  local size; size=$(wc -c < "$2")
  awk -v n="$1" -v s="$size" -v i="$INPUT" -v a="$3" -v b="$4" \
    'BEGIN{printf "| %s | %d | %.1f%% | %.2f |\n", n, s, 100*s/i, b-a}'
}

for preset in --fast --normal --max; do
  s=$(now); "$MZ" create -j "$J" "$preset" "$TMP/m.mzg" "$DATA" >/dev/null; e=$(now)
  row "zzipp $preset" "$TMP/m.mzg" "$s" "$e"; rm -f "$TMP/m.mzg"
done
s=$(now); "$MZ" create -j "$J" --max --dict 64m --block 256m "$TMP/m.mzg" "$DATA" >/dev/null; e=$(now)
row "zzipp --max --dict 64m --block 256m" "$TMP/m.mzg" "$s" "$e"; rm -f "$TMP/m.mzg"

s=$(now); "$MZ" create -j "$J" --max --dict 64m --block 256m "$TMP/m.mzg" "$DATA" >/dev/null; "$MZ" export-zip "$TMP/m.mzg" "$TMP/m.zip" >/dev/null; e=$(now)
row "zzipp export-zip (from --max)" "$TMP/m.zip" "$s" "$e"; rm -f "$TMP/m.mzg" "$TMP/m.zip"

if command -v zip >/dev/null; then
  s=$(now); (cd "$PARENT" && zip -qr9 "$TMP/z.zip" "$NAME"); e=$(now); row "zip -9" "$TMP/z.zip" "$s" "$e"
fi
if command -v xz >/dev/null; then
  s=$(now); tar -C "$PARENT" -cf - "$NAME" | xz -9e -T"$J" > "$TMP/t.tar.xz"; e=$(now); row "tar + xz -9e" "$TMP/t.tar.xz" "$s" "$e"
fi
SEVEN=$(command -v 7zz || command -v 7z || command -v 7za || true)
if [ -n "$SEVEN" ]; then
  s=$(now); (cd "$PARENT" && "$SEVEN" a -t7z -mx=9 -ms=on -mmt="$J" "$TMP/s.7z" "$NAME" >/dev/null); e=$(now); row "7z -mx=9 (solid)" "$TMP/s.7z" "$s" "$e"
fi
if command -v rar >/dev/null; then
  s=$(now); (cd "$PARENT" && rar a -r -m5 -s -idq "$TMP/r.rar" "$NAME"); e=$(now); row "rar -m5 -s" "$TMP/r.rar" "$s" "$e"
fi
