#!/bin/sh
# test_ppm6.sh — régression pour la méthode 6 (PPMd var. I = `Ppmd8`) de 2.0,
# décodeur **et** encodeur.
#
# Décodage : le corpus ne contient que des fichiers « courts » pour la méthode 6
# (le plus gros, `code.c`, fait 45 Ko) : avec une arène de 32 Mo, les chemins de
# recyclage (CutOff / RestoreModel / GlueFreeBlocks) ne sont jamais atteints. Ce
# script génère donc, avec l'oracle, des archives PPMd ciblées :
#   - texte de ~600 Ko (beaucoup de contextes, remises à l'échelle),
#   - mémoire volontairement petite (-PPMM4) => arène recyclée en plein vol,
#   - archive solide à deux fichiers (le modèle PPMd persiste d'un fichier à
#     l'autre : c'est le cas des archives `nested_*` du corpus).
#
# Encodage : nos flux doivent être **bit-identiques** à ceux de l'oracle
# (`sqc -fmt sqx2 -m5` : ordre 6, 32 Mo) et, à l'ordre 8, à ceux de la GUI
# (`packed_corpus/*_v2.0_normal.sqx`) ; nos archives doivent s'extraire par
# `sqc` (interopérabilité réelle) et par notre propre décodeur.
# Usage: sh tools/test_ppm6.sh
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
    echo "oracle sqc not found; please install in _work/tools/bin/sqc"
    exit 1
fi
SQC_ABS="$SQC"
export SQXROOT="$ROOT"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# même_charge utile : compare les charges utiles (flux compressés) de deux
# archives, sans tenir compte du conteneur ni des métadonnées.
meme_charge() {
    python3 - "$1" "$2" <<'PY'
import os, sys
sys.path.insert(0, os.path.join(os.environ['SQXROOT'], 'tests'))
sys.path.insert(0, os.path.join(os.environ['SQXROOT'], 'tools'))
sys.path.insert(0, os.path.join(os.environ['SQXROOT'], '_work/tools'))
import sqxdump as D

def charge(path):
    data = open(path, 'rb').read()
    off = 0
    while off < len(data):
        b = D.read_block(data, off)
        if b is None:
            break
        if b.type == D.BLOCK_FILE:
            f = D.parse_filehead(b, data, b.off + 7)
            return data[f['data_off']:f['after_off']]
        off += b.size
    return None

a, b = charge(sys.argv[1]), charge(sys.argv[2])
if a is None or b is None:
    sys.exit(2)
if a != b:
    print('    charge utile %d vs %d octets, premier ecart a %s' %
          (len(a), len(b), next((i for i in range(min(len(a), len(b)))
                                 if a[i] != b[i]), 'la fin')))
    sys.exit(1)
sys.exit(0)
PY
}

# Texte déterministe, bien plus long que ceux du corpus (phrase répétée avec
# des variantes : les contextes d'ordre élevé sont nombreux). Le deuxième
# fichier partage une bonne partie du vocabulaire pour tester la persistance
# du modèle dans une archive solide.
python3 - "$TMP" <<'PY'
import random, sys, os
out = sys.argv[1]
random.seed(3)
words = ['compression', 'range', 'coder', 'modele', 'contexte', 'symbole',
         'frequence', 'arbre', 'SQX', 'PPMd', 'texte', 'prediction',
         'escamotage', 'algorithme', 'memoire', 'adaptation']
lines = []
for i in range(9000):
    lines.append(' '.join(random.choice(words) for _ in range(random.randrange(4, 14))))
    if i % 17 == 0:
        lines.append('')
open(os.path.join(out, 'big.txt'), 'w').write('\n'.join(lines) + '\n')
lines = []
for i in range(1500):
    lines.append('suite %d : ' % i +
                 ' '.join(random.choice(words) for _ in range(random.randrange(3, 9))))
open(os.path.join(out, 'suite.txt'), 'w').write('\n'.join(lines) + '\n')
PY

ok=0; ko=0

# ---------------------------------------------------------------- décodeur ---

# 1. un seul fichier, mémoire normale puis réduite (recyclage de l'arène)
for mem in 64 4; do
    a="$TMP/m$mem.sqx"
    if ! "$SQC" a -fmt sqx2 -m5 -PPMM$mem "$a" "$TMP/big.txt" >/dev/null 2>&1; then
        echo "  mem=$mem Mo: compression oracle impossible"
        ko=$((ko + 1)); continue
    fi
    rm -rf "$TMP/x"; mkdir -p "$TMP/x"
    if ! $SQX extract "$TMP/x" "$a" >/dev/null 2>&1; then
        echo "  mem=$mem Mo: extraction impossible"
        ko=$((ko + 1)); continue
    fi
    got=$(find "$TMP/x" -type f | head -1)
    if [ -z "$got" ] || ! cmp -s "$got" "$TMP/big.txt"; then
        echo "  mem=$mem Mo: contenu different"
        ko=$((ko + 1)); continue
    fi
    ok=$((ok + 1))
done

# 2. archive solide : deux fichiers texte (le modèle PPMd continue d'un
#    fichier au suivant)
a="$TMP/solid.sqx"
if "$SQC" a -fmt sqx2 -m5 -PPMM4 "$a" "$TMP/big.txt" "$TMP/suite.txt" >/dev/null 2>&1; then
    rm -rf "$TMP/x"; mkdir -p "$TMP/x"
    if $SQX extract "$TMP/x" "$a" >/dev/null 2>&1; then
        n=0
        for base in big suite; do
            got=$(find "$TMP/x" -type f -name "$base.txt" | head -1)
            if [ -n "$got" ] && cmp -s "$got" "$TMP/$base.txt"; then
                n=$((n + 1))
            fi
        done
        if [ "$n" -eq 2 ]; then
            ok=$((ok + 1))
        else
            echo "  solide: $n/2 fichier(s) conforme(s)"
            ko=$((ko + 1))
        fi
    else
        echo "  solide: extraction impossible"
        ko=$((ko + 1))
    fi
else
    echo "  solide: compression oracle impossible"
    ko=$((ko + 1))
fi

# ---------------------------------------------------------------- encodeur ---

# 3. bit-identité avec l'oracle (ordre 6 / 32 Mo) sur les textes du corpus et
#    le texte généré, puis extraction de nos archives par `sqc`.
for f in corpus/hello.txt corpus/eng.txt corpus/code.c "$TMP/big.txt"; do
    b=$(basename "$f")
    ours="$TMP/enc-$b.sqx"
    th=$TMP/o-$b.sqx
    rm -f "$ours" "$th"
    if ! $SQX create "$ours" --fmt=20 --method=6 "$f" >/dev/null 2>&1; then
        echo "  $b: compression impossible"
        ko=$((ko + 1)); continue
    fi
    if ! "$SQC" a -fmt sqx2 -m5 "$th" "$f" >/dev/null 2>&1; then
        echo "  $b: compression oracle impossible"
        ko=$((ko + 1)); continue
    fi
    if ! meme_charge "$ours" "$th"; then
        echo "  $b: flux different de l'oracle"
        ko=$((ko + 1)); continue
    fi
    # notre archive doit s'extraire par l'oracle...
    rm -rf "$TMP/sx"; mkdir -p "$TMP/sx"
    if ! (cd "$TMP/sx" && "$SQC_ABS" x "$ours" >/dev/null 2>&1); then
        echo "  $b: l'oracle n'extrait pas notre archive"
        ko=$((ko + 1)); continue
    fi
    got=$(find "$TMP/sx" -type f -name "$b" | head -1)
    if [ -z "$got" ] || ! cmp -s "$got" "$f"; then
        echo "  $b: contenu extrait par l'oracle different"
        ko=$((ko + 1)); continue
    fi
    ok=$((ok + 1))
done

# 4. ordre 8 (le réglage de la GUI 5.62) : notre flux doit être identique à
#    celui de `packed_corpus/<nom>_v2.0_normal.sqx`.
for spec in hello:corpus/hello.txt eng:corpus/eng.txt code:corpus/code.c; do
    base=${spec%%:*}; src=${spec#*:}
    gui="packed_corpus/${base}_v2.0_normal.sqx"
    ours="$TMP/g8-$base.sqx"
    [ -f "$gui" ] && [ -f "$src" ] || continue
    rm -f "$ours"
    if ! $SQX create "$ours" --fmt=20 --method=6 --ppm-order=8 "$src" >/dev/null 2>&1; then
        echo "  $base (ordre 8): compression impossible"
        ko=$((ko + 1)); continue
    fi
    if ! meme_charge "$ours" "$gui"; then
        echo "  $base (ordre 8): flux different de la GUI"
        ko=$((ko + 1)); continue
    fi
    ok=$((ok + 1))
done

# 5. arène réduite à l'encodage : notre décodeur doit relire notre flux, et
#    `sqc` aussi (les deux moitiés du codec recyclent l'arène en plein vol).
a="$TMP/enc4.sqx"
if $SQX create "$a" --fmt=20 --method=6 --ppm-mem=4 "$TMP/big.txt" >/dev/null 2>&1; then
    rm -rf "$TMP/x"; mkdir -p "$TMP/x"
    if $SQX extract "$TMP/x" "$a" >/dev/null 2>&1; then
        got=$(find "$TMP/x" -type f | head -1)
        if [ -n "$got" ] && cmp -s "$got" "$TMP/big.txt"; then
            rm -rf "$TMP/sx"; mkdir -p "$TMP/sx"
            if (cd "$TMP/sx" && "$SQC_ABS" x "$a" >/dev/null 2>&1); then
                got=$(find "$TMP/sx" -type f -name 'big.txt' | head -1)
                if [ -n "$got" ] && cmp -s "$got" "$TMP/big.txt"; then
                    ok=$((ok + 1))
                else
                    echo "  mem=4 Mo (encodage): contenu extrait par l'oracle different"
                    ko=$((ko + 1))
                fi
            else
                echo "  mem=4 Mo (encodage): l'oracle n'extrait pas notre archive"
                ko=$((ko + 1))
            fi
        else
            echo "  mem=4 Mo (encodage): round-trip different"
            ko=$((ko + 1))
        fi
    else
        echo "  mem=4 Mo (encodage): extraction impossible"
        ko=$((ko + 1))
    fi
else
    echo "  mem=4 Mo (encodage): compression impossible"
    ko=$((ko + 1))
fi

# 6. cas limites de l'encodeur : fichier vide (aucun symbole) et données
#    incompressibles (beaucoup d'échappements et de contextes neufs).
: > "$TMP/vide.bin"
head -c 40000 /dev/urandom > "$TMP/bruit.bin" 2>/dev/null || \
    python3 -c "import random,sys;random.seed(5);open(sys.argv[1],'wb').write(bytes(random.randrange(256) for _ in range(40000)))" "$TMP/bruit.bin"
for b in vide bruit; do
    a="$TMP/lim-$b.sqx"
    rm -f "$a"
    if ! $SQX create "$a" --fmt=20 --method=6 "$TMP/$b.bin" >/dev/null 2>&1; then
        echo "  $b: compression impossible"
        ko=$((ko + 1)); continue
    fi
    rm -rf "$TMP/x"; mkdir -p "$TMP/x"
    if ! $SQX extract "$TMP/x" "$a" >/dev/null 2>&1 || \
       ! cmp -s "$TMP/x/$b.bin" "$TMP/$b.bin"; then
        echo "  $b: round-trip different"
        ko=$((ko + 1)); continue
    fi
    rm -rf "$TMP/sx"; mkdir -p "$TMP/sx"
    if (cd "$TMP/sx" && "$SQC_ABS" x "$a" >/dev/null 2>&1) && \
       cmp -s "$TMP/sx/$b.bin" "$TMP/$b.bin"; then
        ok=$((ok + 1))
    else
        echo "  $b: l'oracle n'extrait pas notre archive"
        ko=$((ko + 1))
    fi
done

echo "ppm6 (methode 6, decodeur + encodeur): $ok conformes, $ko echec(s)"
[ "$ko" -eq 0 ]
