#!/bin/sh
# test_bitexact.sh — encoder bit-identity test: each LZ/Huffman stream in the
# corpus is decoded and re-encoded from the IR; the result must be byte-identical
# to the original stream (v1.1 and v2.0).
#
# Archives using out-of-scope methods (5 audio, 6 PPMd, 7 ultra) are ignored;
# extracting an archive without an exploitable LZ stream is not counted as failure.
# Usage: sh tests/test_bitexact.sh
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

out=$("$SQX" check packed_corpus/*.sqx 2>&1 || true)
echo "$out" | grep -v "^repack:" | sed 's/^/  /' || true

ident=$(echo "$out" | sed -n 's/.*: \([0-9]*\) \(flux ré-encodés\|re-encoded streams\).*/\1/p')
diff=$(echo "$out"  | sed -n 's/.*, \([0-9]*\) divergence(s).*/\1/p')
err=$(echo "$out"   | sed -n 's/.*divergence(s), \([0-9]*\) \(erreur(s)\|error(s)\).*/\1/p')

echo "bit-identity: $ident identical streams, $diff divergence(s), $err non-decoded stream(s)"
[ "$diff" = "0" ]
