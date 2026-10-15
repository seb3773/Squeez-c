#!/bin/sh
# test_large.sh — regression for long distances: with a 4MB dictionary,
# extra distance bits exceed 16 bits and MIN_LEN3/MIN_LEN4 thresholds become
# reachable. Verified for both formats (1.1 and 2.0):
#   1. round-trip by this decoder (byte-by-byte),
#   2. extraction by oracle `sqc` (real interoperability).
# Usage: sh tests/test_large.sh
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

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
    echo "oracle sqc not found; skipping oracle verification"
    SQC=""
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Deterministic data:
#  - 400,000 random bytes (incompressible -> literals + pseudo-matches),
#  - base repeated 3 times -> matches at distance 400,000 (> MIN_LEN4),
#  - small snippets        -> short matches at long distances.
python3 - "$TMP" <<'EOF'
import random, sys, os
tmp = sys.argv[1]
random.seed(11)
base = bytes(random.randrange(256) for _ in range(400000))
open(os.path.join(tmp, 'big.bin'), 'wb').write(base + base + base)
snip = bytearray(base)
for off, ln in [(0, 5), (1000, 6), (2000, 10), (3000, 20), (5000, 100),
                (20000, 250), (40000, 60), (70000, 33), (90000, 9),
                (120000, 150)]:
    snip += base[off:off + ln]
    snip += bytes(random.randrange(256) for _ in range(37))
open(os.path.join(tmp, 'snip.bin'), 'wb').write(bytes(snip))
EOF

ok=0; ko=0
for f in big snip; do
    for fmt in 11 20; do
        for m in 1 4; do
            tag="$f fmt=$fmt m=$m"
            a="$TMP/$f-$fmt-$m.sqx"
            rm -f "$a"
            if ! "$SQX" create "$a" --fmt=$fmt --method=$m --dict=4096 \
                        "$TMP/$f.bin" >/dev/null 2>&1; then
                echo "  $tag: compression failed"
                ko=$((ko + 1)); continue
            fi
            rm -rf "$TMP/xd"; mkdir -p "$TMP/xd"
            if ! "$SQX" extract "$TMP/xd" "$a" >/dev/null 2>&1 ||
               ! cmp -s "$TMP/xd/$f.bin" "$TMP/$f.bin"; then
                echo "  $tag: round-trip content differs"
                ko=$((ko + 1)); continue
            fi
            if [ -n "$SQC" ]; then
                rm -rf "$TMP/xe"; mkdir -p "$TMP/xe"
                (cd "$TMP/xe" && "$SQC" x -o "$a" >/dev/null 2>&1) || true
                g=$(find "$TMP/xe" -type f -name "$f.bin" | head -1)
                if [ -z "$g" ] || ! cmp -s "$g" "$TMP/$f.bin"; then
                    echo "  $tag: oracle sqc extracted different content"
                    ko=$((ko + 1)); continue
                fi
            fi
            ok=$((ok + 1))
        done
    done
done

echo "long distances: $ok combination(s) validated, $ko failure(s)"
[ "$ko" -eq 0 ]
