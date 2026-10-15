#!/bin/sh
# Test M0: C parser reads all samples without CRC error
# and matches sizes/names/methods from the reference Python dumper.
# Usage: sh tests/test_container.sh
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

fails=0
total=0

for f in packed_corpus/*.sqx packed_corpus/*.exe; do
  total=$((total + 1))
  if ! "$SQX" test "$f" >/dev/null 2>&1; then
    echo "CRC FAILURE: $f"
    fails=$((fails + 1))
  fi
done

# Cross-comparison C / Python on sizes and methods
DUMPER="$ROOT/tests/sqxdump.py"
[ -f "$DUMPER" ] || DUMPER="$ROOT/_work/tools/sqxdump.py"

python3 "$DUMPER" --compat packed_corpus/*.sqx > /tmp/sqxdump_ref.txt
python3 - "$SQX" <<'EOF' > /tmp/sqxdump_c.txt
import subprocess, glob, os, sys, struct, re
sqx_bin = sys.argv[1]
for f in sorted(glob.glob('packed_corpus/*.sqx')):
    out = subprocess.run([sqx_bin, 'info', f], capture_output=True, text=True).stdout
    m = re.search(r"name='([^']*)' fmt=(\S+) method=0x(\w\w)", out)
    c = re.search(r"comp=(\d+) orig=(\d+)", out)
    print("%-34s %-4s 0x%s %s %s" % (os.path.basename(f), m.group(2), m.group(3), c.group(1), c.group(2)))
EOF
# Compare: same (method, comp, orig) on both sides
python3 - <<'EOF'
import re
pat = re.compile(r'^(\S+\.sqx)\s+\S+\s+(0x\w\w)\s+(\d+)\s+(\d+)\s*$')
def norm(fn):
    d = {}
    for line in open(fn):
        m = pat.match(line.strip())
        if m: d[m.group(1)] = (m.group(2), m.group(3), m.group(4))
    return d
ref = norm('/tmp/sqxdump_ref.txt')
cur = norm('/tmp/sqxdump_c.txt')
diff = [k for k in ref if k not in cur or ref[k] != cur.get(k)]
print("Cross-comparison Python<->C: %d archives Python, %d C, %d divergences" % (len(ref), len(cur), len(diff)))
for k in diff[:10]: print("  ", k, ref[k], cur.get(k))
raise SystemExit(1 if diff else 0)
EOF

echo "M0 container: $total archives tested, $fails CRC failure(s), cross-comparison OK"
[ "$fails" -eq 0 ]

# Verification of -1 (bare) and -u (unix) CLI options
bare_cnt=$("$SQX" list -1 packed_corpus/nested_v1.1_fast.sqx | wc -l)
if [ "$bare_cnt" -ne 10 ]; then
    echo "test_container: -1 bare output expected 10 lines, got $bare_cnt"
    exit 1
fi
unix_out=$("$SQX" list -1 -u corpus/Noise_v2.0_normal.sqx)
if [ "$unix_out" != "noise.wav" ]; then
    echo "test_container: -u unix output expected 'noise.wav', got '$unix_out'"
    exit 1
fi
