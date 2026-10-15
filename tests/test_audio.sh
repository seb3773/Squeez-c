#!/bin/sh
# test_audio.sh — extrait les archives de méthode 5 (audio) et compare les WAV.
#
# Le codec 1.1 (cx_audio.c) est porté : 1x16/2x16 (trames de 384 échantillons,
# prédiction polynomiale) et 1x8/2x8 (deltas Huffman). Le codec 2.0 est porté
# **en entier** : canaux 16 bits (trames de 4608 échantillons, paquets FIR/Rice,
# prédicteurs polynomiaux — notes/07-audio.md §7.5) et canaux 8 bits (range
# coder + 256 contextes de modèles de demi-octets + prédicteur delta adaptatif —
# §7.6), plus l'encodeur 2.0 16 bits (§7.7).
# Usage: sh tools/test_audio.sh
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

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Plus aucune limitation attendue sur le corpus : une archive 2.0 qui échoue
# est désormais une régression, pas un échec connu.
XFAIL=""
ok=0; ko=0; xfail=0; none=0
for f in corpus/*.sqx; do
    [ -e "$f" ] || continue
    meth=$($SQX info "$f" | grep -o 'method=0x..' | sort -u | tail -1)
    [ "$meth" = "method=0x05" ] || continue
    name=$(basename "$f" .sqx)

    rm -rf "$TMP/x"; mkdir -p "$TMP/x"
    if ! $SQX extract "$TMP/x" "$f" >/dev/null 2>&1; then
        case " $XFAIL " in
            *" $name "*)
                echo "  $name: limitation connue (codec audio 2.0)"
                xfail=$((xfail + 1)); continue ;;
        esac
        echo "  $name: extraction impossible"
        ko=$((ko + 1)); continue
    fi

    bad=""; nfile=0
    for g in $(find "$TMP/x" -type f | sort); do
        nfile=$((nfile + 1))
        rel=${g#"$TMP/x/"}
        if [ ! -f "corpus/$rel" ]; then
            bad="pas d'original pour $rel"
        elif ! cmp -s "$g" "corpus/$rel"; then
            bad="contenu different de corpus/$rel"
        fi
    done
    [ -n "$bad" ] || [ "$nfile" -gt 0 ] || bad="aucun fichier extrait"
    if [ -n "$bad" ]; then
        echo "  $name: $bad"
        ko=$((ko + 1))
    else
        ok=$((ok + 1))
    fi
done

# --- round-trip sur les 4 combinaisons de codec (mono/stereo, 8/16 bits) ---
# Les archives du corpus ne couvrent que le 16 bits mono : on genere les
# autres cas avec l'oracle (comme tools/gen_samples.sh) quand il est present.
if [ -x "$ROOT/tools/bin/sqc" ]; then
    SQC="$ROOT/tools/bin/sqc"
elif [ -x "$ROOT/_work/tools/bin/sqc" ]; then
    SQC="$ROOT/_work/tools/bin/sqc"
else
    SQC=""
fi
gen_ok=0; gen_ko=0
gen20b_ok=0; gen20b_ko=0
if [ -x "$SQC" ] && command -v python3 >/dev/null 2>&1; then
    python3 - "$TMP" <<'PY'
import math, random, struct, sys, os
out = sys.argv[1]
random.seed(7)
for name, ch, bits in (('m8', 1, 8), ('s8', 2, 8), ('m16', 1, 16), ('s16', 2, 16)):
    data = bytearray()
    for i in range(2000):
        for c in range(ch):
            v = math.sin(i * 0.03 + c) * 0.5 + (random.random() - 0.5) * 0.05
            if bits == 8:
                data.append(int(max(0, min(255, 128 + v * 100))))
            else:
                data += struct.pack('<h', int(max(-32768, min(32767, v * 20000))))
    hdr = (b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ' +
           struct.pack('<IHHIIHH', 16, 1, ch, 8000, 8000 * ch * bits // 8,
                       ch * bits // 8, bits) + b'data' + struct.pack('<I', len(data)))
    open(os.path.join(out, name + '.wav'), 'wb').write(hdr + bytes(data))
# mono 8 bits avec un nombre impair d'echantillons et une queue non audio :
# le reste impair doit partir dans lFileTail (le 8 bits 2.0 exige
# uRawDataSize % cChannels == 0).
data = bytearray(int(128 + 80 * math.sin(i * 0.02)) for i in range(1001))
hdr = (b'RIFF' + struct.pack('<I', 36 + len(data) + 5) + b'WAVEfmt ' +
       struct.pack('<IHHIIHH', 16, 1, 1, 8000, 8000, 1, 8) +
       b'data' + struct.pack('<I', len(data)))
open(os.path.join(out, 't8.wav'), 'wb').write(hdr + bytes(data) + b'JUNK\x00')
PY
    for f in m8 s8 m16 s16; do
        rm -f "$TMP/$f.sqx"
        if ! "$SQC" a -fmt sqx1 -m5 "$TMP/$f.sqx" "$TMP/$f.wav" >/dev/null 2>&1; then
            echo "  $f: compression oracle impossible"
            gen_ko=$((gen_ko + 1)); continue
        fi
        rm -rf "$TMP/g"; mkdir -p "$TMP/g"
        if ! $SQX extract "$TMP/g" "$TMP/$f.sqx" >/dev/null 2>&1; then
            echo "  $f: extraction impossible"
            gen_ko=$((gen_ko + 1)); continue
        fi
        got=$(find "$TMP/g" -type f | head -1)
        if [ -z "$got" ] || ! cmp -s "$got" "$TMP/$f.wav"; then
            echo "  $f ($f.wav): contenu different"
            gen_ko=$((gen_ko + 1))
        else
            gen_ok=$((gen_ok + 1))
        fi
    done
    echo "audio: $gen_ok/4 combinaison(s) mono/stereo x 8/16 bits conforme(s)"

    # --- codec 2.0 : les 4 combinaisons (mono/stéréo x 8/16 bits) doivent être
    # décodées bit-exactement (cf. notes/07-audio.md §7.5 et §7.6).
    gen20_ok=0; gen20_ko=0
    for spec in m8:1:8 s8:2:8 m16:1:16 s16:2:16; do
        f=${spec%%:*}; rest=${spec#*:}; ch=${rest%%:*}; w=${rest#*:}
        rm -f "$TMP/$f.20.sqx"
        if ! "$SQC" a -fmt sqx2 -m5 -AU2 -AUC$ch -AUW$w \
                "$TMP/$f.20.sqx" "$TMP/$f.wav" >/dev/null 2>&1; then
            echo "  $f (2.0): compression oracle impossible"
            gen20_ko=$((gen20_ko + 1)); continue
        fi
        rm -rf "$TMP/g"; mkdir -p "$TMP/g"
        if $SQX extract "$TMP/g" "$TMP/$f.20.sqx" >/dev/null 2>&1; then
            got=$(find "$TMP/g" -type f | head -1)
            if [ -n "$got" ] && cmp -s "$got" "$TMP/$f.wav"; then
                gen20_ok=$((gen20_ok + 1)); continue
            fi
        fi
        echo "  $f (2.0): contenu different"
        gen20_ko=$((gen20_ko + 1))
    done
    echo "audio 2.0: $gen20_ok/4 conforme(s), $gen20_ko echec(s)"
    ko=$((ko + gen20_ko))

    # --- 8 bits 2.0 en mode *normal* (l'oracle garde l'en-tete WAV comme
    # lSkipSize et ne code que les donnees) + cas a queue : chemin different du
    # mode force (-AU2) ci-dessus, qui code le fichier entier sans en-tete.
    gen8_ok=0; gen8_ko=0
    for spec in m8:1 s8:2 t8:1; do
        f=${spec%%:*}; ch=${spec#*:}
        rm -f "$TMP/$f.20auto.sqx"
        if ! "$SQC" a -o -fmt sqx2 -m5 "$TMP/$f.20auto.sqx" "$TMP/$f.wav" >/dev/null 2>&1; then
            echo "  $f (2.0 auto): compression oracle impossible"
            gen8_ko=$((gen8_ko + 1)); continue
        fi
        [ "$(./bin/sqx info "$TMP/$f.20auto.sqx" | grep -o 'method=0x..' | tail -1)" = "method=0x05" ] || {
            echo "  $f (2.0 auto): l'oracle n'a pas choisi l'audio"; gen8_ko=$((gen8_ko + 1)); continue; }
        rm -rf "$TMP/g"; mkdir -p "$TMP/g"
        got=""
        $SQX extract "$TMP/g" "$TMP/$f.20auto.sqx" >/dev/null 2>&1 && \
            got=$(find "$TMP/g" -type f | head -1)
        if [ -n "$got" ] && cmp -s "$got" "$TMP/$f.wav"; then
            gen8_ok=$((gen8_ok + 1))
        else
            echo "  $f (2.0 auto): contenu different"
            gen8_ko=$((gen8_ko + 1))
        fi
    done
    echo "audio 2.0 8 bits (mode normal, en-tete + queue): $gen8_ok/3 conforme(s)"
    ko=$((ko + gen8_ko))

    # --- codec 2.0 (16 bits) : chemins rares, qu'un signal « lisse » générique
    # n'exerce pas — entrelacement icd != 0, mode 0 à prédicteur adaptatif,
    # corps à largeur fixe (pleine échelle). Ces cas ont réellement caché des
    # bogues (prédicteur adaptatif, entrelaceur propre au 2.0).
    python3 - "$TMP" <<'PY'
import math, random, struct, sys, os
out = sys.argv[1]
random.seed(11)
def write(name, ch, pairs):
    data = bytearray()
    for l, r in pairs:
        data += struct.pack('<hh', max(-32768, min(32767, l)), max(-32768, min(32767, r)))
    hdr = (b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ' +
           struct.pack('<IHHIIHH', 16, 1, ch, 8000, 8000 * ch * 2, ch * 2, 16) +
           b'data' + struct.pack('<I', len(data)))
    open(os.path.join(out, name + '.wav'), 'wb').write(hdr + bytes(data))
N = 3000
base = [int(9000 * math.sin(i * 0.02)) for i in range(N)]
write('ic_a', 2, [(base[i], base[i]) for i in range(N)])
write('ic_b', 2, [(base[i], -base[i]) for i in range(N)])
write('ic_c', 2, [(base[i], base[i] // 2 + (i % 7 - 3)) for i in range(N)])
write('ic_d', 2, [(base[i], (base[i] + base[i - 1]) // 2) for i in range(1, N)])
write('loudm', 1, [(random.randint(-32768, 32767) if i % 3 else
                    (30000 if i % 6 < 3 else -30000), 0) for i in range(4000)])
PY
    gen20b_ok=0; gen20b_ko=0
    for spec in ic_a:2 ic_b:2 ic_c:2 ic_d:2 loudm:1; do
        f=${spec%%:*}; ch=${spec#*:}
        rm -f "$TMP/$f.20.sqx"
        if ! "$SQC" a -o -fmt sqx2 -m5 -AU2 -AUC$ch -AUW16 \
                "$TMP/$f.20.sqx" "$TMP/$f.wav" >/dev/null 2>&1; then
            echo "  $f (2.0): compression oracle impossible"
            gen20b_ko=$((gen20b_ko + 1)); continue
        fi
        rm -rf "$TMP/g"; mkdir -p "$TMP/g"
        got=""
        $SQX extract "$TMP/g" "$TMP/$f.20.sqx" >/dev/null 2>&1 && \
            got=$(find "$TMP/g" -type f | head -1)
        if [ -n "$got" ] && cmp -s "$got" "$TMP/$f.wav"; then
            gen20b_ok=$((gen20b_ok + 1))
        else
            echo "  $f (2.0, chemin rare): contenu different"
            gen20b_ko=$((gen20b_ko + 1))
        fi
    done
    echo "audio 2.0 (entrelacement icd, pred adaptatif, pleine echelle):"
    echo "  $gen20b_ok/5 conforme(s)"
    ko=$((ko + gen20b_ko))

    # --- encodeur audio (methode 5, 2.0, canaux 16 bits) — nos archives doivent
    # etre relues **bit-exactement** par notre decodeur *et* par l'oracle : c'est
    # ce second controle qui prouve que le flux produit est valide (encodage
    # non bit-identique a celui de Squeez, cf. src/audio.c).
    python3 - "$TMP" <<'PY'
import struct, sys, os
out = sys.argv[1]
def wav(name, data, ch=1, bits=16):
    hdr = (b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ' +
           struct.pack('<IHHIIHH', 16, 1, ch, 8000, 8000 * ch * bits // 8,
                       ch * bits // 8, bits) + b'data' +
           struct.pack('<I', len(data)))
    open(os.path.join(out, name + '.wav'), 'wb').write(hdr + data)
wav('t_one', b'\x34\x12')                  # 1 echantillon : tout en queue
wav('t_two', b'\x34\x12\x78\x56')          # exactement une trame
wav('t_odd', b'\x01\x02\x03\x04\x05')      # reste impair
wav('t_empty', b'')                        # aucune donnee
wav('t_stereo_odd', b'\x01\x00\x02\x00\x03\x00', ch=2)
# WAV multi-trames (> 4608 echantillons) : 15 trames, dont une partielle
import math, random
wav('t_multi', b''.join(struct.pack('<h', int(8000 * math.sin(i * 0.01)))
                        for i in range(12000)))
# impulsionnel : residuels enormes et rares (le quotient unaire depasse la
# taille d'un mot de 32 bits avec les petits k, cf. aud2_put_zero_run)
wav('t_sparse', b''.join(struct.pack('<h', (32000 if i % 200 < 100 else -32000)
                                     if i % 97 == 0 else 0)
                         for i in range(6000)))
# pleine echelle aleatoire, stereo (deux canaux independants)
random.seed(3)
wav('t_loud', b''.join(struct.pack('<hh', random.randint(-32768, 32767),
                                   random.randint(-32768, 32767))
                       for _ in range(5000)), ch=2)
PY
    enc_ok=0; enc_ko=0
    enc_sources="$TMP/m16.wav $TMP/s16.wav $TMP/ic_a.wav $TMP/ic_b.wav"
    enc_sources="$enc_sources $TMP/ic_c.wav $TMP/ic_d.wav $TMP/loudm.wav"
    enc_sources="$enc_sources $TMP/t_one.wav $TMP/t_two.wav $TMP/t_odd.wav"
    enc_sources="$enc_sources $TMP/t_empty.wav $TMP/t_stereo_odd.wav $TMP/t_multi.wav"
    enc_sources="$enc_sources $TMP/t_sparse.wav $TMP/t_loud.wav"
    # le PCM 8 bits fait désormais partie du chemin encodeur
    python3 -c "import struct,sys; open('$TMP/bad8.wav','wb').write(b'RIFF'+struct.pack('<I',36+100)+b'WAVEfmt '+struct.pack('<IHHIIHH',16,1,1,8000,8000,1,8)+b'data'+struct.pack('<I',100)+b'x'*100)"
    enc_sources="$enc_sources $TMP/bad8.wav"
    [ -f corpus/Noise.wav ] && enc_sources="$enc_sources corpus/Noise.wav"
    # fixtures adversariales : cases limites de l'encodeur AUDIO 2.0
    # (alternance extrême, impulsion unitaire, tailles non alignées, silence,
    # DC, rampe pleine échelle - générées par tools/gen_adversarial.sh)
    if [ -f corpus/x_alt16.wav ]; then
        for adv in corpus/x_*.wav; do [ -e "$adv" ] && enc_sources="$enc_sources $adv"; done
    fi
    for src in $enc_sources; do
        f=$(basename "$src")
        rm -rf "$TMP/e" "$TMP/round" "$TMP/refx"
        mkdir -p "$TMP/e" "$TMP/round" "$TMP/refx"
        if ! $SQX create "$TMP/e/$f.sqx" --fmt=20 --method=5 "$src" >/dev/null 2>&1; then
            echo "  $f (encodeur): compression impossible"
            enc_ko=$((enc_ko + 1)); continue
        fi
        if ! $SQX extract "$TMP/round" "$TMP/e/$f.sqx" >/dev/null 2>&1 || \
           ! cmp -s "$TMP/round/$f" "$src"; then
            echo "  $f (encodeur): notre decodeur ne relit pas le meme contenu"
            enc_ko=$((enc_ko + 1)); continue
        fi
        (cd "$TMP/refx" && "$SQC" x "../e/$f.sqx" >/dev/null 2>&1)
        if cmp -s "$TMP/refx/$f" "$src"; then
            enc_ok=$((enc_ok + 1))
        else
            echo "  $f (encodeur): l'oracle relit un contenu different"
            enc_ko=$((enc_ko + 1))
        fi
    done
    echo "audio 2.0 (encodeur, relecture par l'oracle): $enc_ok conforme(s), $enc_ko echec(s)"
    ko=$((ko + enc_ko))

    # entrees non exploitables : refus explicite (pas de plantage, pas d'archive)
    rej_ok=0; rej_ko=0
    python3 -c "import struct,sys; pass"
    head -c 200 /dev/urandom > "$TMP/bad.bin"
    for bad in "$TMP/bad.bin" corpus/Noise_v2.0_normal.sqx; do
        if $SQX create "$TMP/bad.sqx" --fmt=20 --method=5 "$bad" >/dev/null 2>&1; then
            echo "  $(basename "$bad"): accepte a tort par l'encodeur audio"
            rej_ko=$((rej_ko + 1))
        else
            rej_ok=$((rej_ok + 1))
        fi
    done
    if $SQX create "$TMP/bad.sqx" --fmt=11 --method=5 "$TMP/t_two.wav" >/dev/null 2>&1; then
        echo "  audio en 1.1: accepte a tort"
        rej_ko=$((rej_ko + 1))
    else
        rej_ok=$((rej_ok + 1))
    fi
    echo "audio 2.0 (refus des entrees non audio): $rej_ok conforme(s), $rej_ko echec(s)"
    ko=$((ko + rej_ko))
else
    echo "audio: round-trip 8/16 bits ignore (oracle tools/bin/sqc absent)"
fi

ko=$((ko + gen_ko + gen20b_ko))

if [ "$ok" -eq 0 ] && [ "$xfail" -eq 0 ] && [ "$gen_ok" -eq 0 ]; then
    echo "audio: aucune archive de methode 5 testee (ignore)"
    exit 0
fi
echo "audio: $ok archive(s) conforme(s), $ko echec(s), $xfail limitation(s) connue(s)"
[ "$ko" -eq 0 ]
