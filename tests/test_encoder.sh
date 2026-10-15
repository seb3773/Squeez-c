#!/bin/sh
# test_encoder.sh — verifies C encoder (src/create.c):
#   1. `sqx create` produces an archive;
#   2. this decoder extracts it bit-identically (byte-by-byte);
#   3. oracle `sqc t` validates CRCs (real interoperability).
# Usage: sh tests/test_encoder.sh
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)

if [ -x "$ROOT/build/sqx" ]; then
    SQX="$ROOT/build/sqx"
elif [ -x "$ROOT/build/linux/sqx" ]; then
    SQX="$ROOT/build/linux/sqx"
elif [ -x "$ROOT/bin/sqx" ]; then
    SQX="$ROOT/bin/sqx"
else
    echo "sqx binary not found; please run ./build.sh or make"
    exit 1
fi

if [ -x "$ROOT/tools/bin/sqc" ]; then
    SQC="$ROOT/tools/bin/sqc"
elif [ -x "$ROOT/_work/tools/bin/sqc" ]; then
    SQC="$ROOT/_work/tools/bin/sqc"
else
    echo "oracle sqc not found; skipping oracle validation"
    SQC=""
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

FILES="$ROOT/corpus/eng.txt $ROOT/corpus/biased.bin $ROOT/corpus/code.c \
$ROOT/corpus/image.raw $ROOT/corpus/records.bin $ROOT/corpus/nested/image24.raw"

ok=0
ko=0
for fmt in 11 20; do
  for m in 1 2 4; do
    for md in 32 256 4096; do
      name="t_${fmt}_${m}_${md}"
      if ! "$SQX" create "$TMP/$name.sqx" --fmt=$fmt --method=$m --dict=$md \
                    $FILES >/dev/null 2>&1; then
        echo "  $name: compression failed"
        ko=$((ko + 1)); continue
      fi
      rm -rf "$TMP/x"; mkdir -p "$TMP/x"
      if ! "$SQX" extract "$TMP/x" "$TMP/$name.sqx" >/dev/null 2>&1; then
        echo "  $name: extraction failed"
        ko=$((ko + 1)); continue
      fi
      bad=0
      for f in $FILES; do
        b=$(basename "$f")
        cmp -s "$TMP/x/$b" "$f" || bad=1
      done
      if [ $bad -ne 0 ]; then
        echo "  $name: round-trip content differs from original"
        ko=$((ko + 1)); continue
      fi
      if [ -n "$SQC" ]; then
        if "$SQC" t "$TMP/$name.sqx" 2>&1 | grep -q "0 Error"; then
          ok=$((ok + 1))
        else
          echo "  $name: oracle sqc rejected archive"
          ko=$((ko + 1))
        fi
      else
        ok=$((ok + 1))
      fi
    done
  done
done

# Size comparison vs oracle (same file, same settings)
if [ -n "$SQC" ]; then
  echo "--- ratio vs oracle (eng.txt, dico 4M) ---"
  for fmt in 11 20; do
    for m in 1 4; do
      ours=$("$SQX" create "$TMP/r.sqx" --fmt=$fmt --method=$m --dict=4096 \
             "$ROOT/corpus/eng.txt" >/dev/null 2>&1; stat -c%s "$TMP/r.sqx")
      opt="-fmt sqx1"
      [ "$fmt" = "20" ] && opt="-fmt sqx2 -PPM0"
      rm -f "$TMP/o.sqx"
      ( cd "$TMP" && "$SQC" a $opt -m$m -md4096 o.sqx "$ROOT/corpus/eng.txt" \
        >/dev/null 2>&1 )
      printf "  fmt=%s m=%s: ours=%s bytes, oracle=%s bytes\n" \
             "$fmt" "$m" "$ours" "$(stat -c%s "$TMP/o.sqx")"
    done
  done
fi

echo "encoder: $ok combination(s) validated (round-trip + oracle), $ko failure(s)"
[ $ko -eq 0 ]
