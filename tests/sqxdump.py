#!/usr/bin/env python3
"""
sqxdump.py - Explorateur / dump de conteneur SQX (v1.1 et v2.0).

But: outil d'analyse pour le reverse engineering du format .sqx.
Ne decode PAS encore les donnees compressees: il decrit le conteneur
(blocs, entetes de fichiers, zones de donnees) et verifie les CRC.

Usage:
  python3 tools/sqxdump.py fichier.sqx [fichier2.sqx ...]
  python3 tools/sqxdump.py --summary packed_corpus/*.sqx
  python3 tools/sqxdump.py --extract-data outdir fichier.sqx
"""
import sys, os, zlib, struct

# ---------------------------------------------------------------- CRC
def crc32_raw(data, crc=0xFFFFFFFF):
    """CRC32 identique a SQX: table 0xEDB88320, init 0xFFFFFFFF, SANS xor final."""
    return zlib.crc32(data, crc ^ 0xFFFFFFFF) ^ 0xFFFFFFFF

def crc16_of(buf):
    return crc32_raw(buf) & 0xFFFF

# ---------------------------------------------------------------- constantes
BLOCK_MAIN      = 0x52  # 'R'
BLOCK_FILE      = 0x44  # 'D'
BLOCK_DIR       = 0x56  # 'V'
BLOCK_END       = 0x53  # 'S'
BLOCK_COMMENT   = 0x43  # 'C'
BLOCK_RDATA     = 0x58  # 'X'
BLOCK_AV        = 0x41  # 'A'
BLOCK_INTERNAL  = 0x49  # 'I'

BLOCK_NAMES = {
    0x52: "MAIN", 0x44: "FILE", 0x56: "DIR", 0x53: "END", 0x43: "COMMENT",
    0x58: "RECOVERY", 0x41: "AUTH", 0x49: "INTERNAL",
}

# METHODES v1.1 (sqx_note.txt / sqx_h.h)
METHODS_V1 = {
    0x00: "stored", 0x01: "normal(1)", 0x02: "good(2)", 0x03: "high(3)",
    0x04: "best(4)", 0x05: "audio", 0x06: "?6", 0x07: "?7",
}
# METHODES v2.0 observees (a completer)
METHODS_V2 = {
    0x00: "stored", 0x01: "fast?", 0x02: "?2", 0x03: "?3", 0x04: "maximum?",
    0x05: "?5", 0x06: "normal?", 0x07: "ultra?", 0x08: "?8",
}

DIC_SIZES = ["32K", "64K", "128K", "256K", "512K", "1024K", "2048K", "4096K"]
DIC_SIZES_V2 = DIC_SIZES + ["8192K", "16384K", "32768K"]
OS_NAMES = {0x14: "Win32", 0x15: "Win64?", 0x0A: "DOS?", 0x00: "?"}

def fmt_method(fmt, m):
    d = METHODS_V1 if fmt == "1.1" else METHODS_V2
    return "0x%02x (%s)" % (m, d.get(m, "?"))

# ---------------------------------------------------------------- helpers
def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]

def hexs(b, maxlen=32):
    s = b[:maxlen].hex(" ")
    return s + (" ..." if len(b) > maxlen else "")

# ---------------------------------------------------------------- parse
class Block:
    def __init__(self, off, crc, btype, flags, size, raw):
        self.off, self.crc, self.type, self.flags, self.size, self.raw = off, crc, btype, flags, size, raw
    @property
    def payload(self):
        return self.raw[7:]  # sans crc(2)+type(1)+flags(2)+size(2)

def read_block(data, off):
    if off + 7 > len(data):
        return None
    crc = u16(data, off)
    btype = data[off + 2]
    flags = u16(data, off + 3)
    size = u16(data, off + 5)
    if size < 7 or off + size > len(data):
        return None
    raw = data[off:off + size]
    b = Block(off, crc, btype, flags, size, raw)
    b.crc_ok = (crc16_of(raw[2:]) == crc)
    return b

def parse_main(b):
    p = b.payload
    out = {}
    out["id"] = p[0:5].decode("latin1")
    out["arc_ver"] = p[5]
    out["vol_num"] = u16(p, 6)
    out["vol_id"] = u32(p, 8)
    out["reserved"] = p[12:18].hex()
    # NB: ce byte vaut TOUJOURS 11 (0x0b) dans les samples, meme en v2.0.
    out["fmt"] = "main-v%d" % out["arc_ver"]
    return out

def parse_filehead(b, data, base):
    """base = offset dans data du champ cCompFlags (= b.off + 7)."""
    o = base
    d = data
    f = {}
    f["comp_flags"] = d[o]
    f["extra_flags"] = u16(d, o + 1)
    f["os_filesys"] = d[o + 3]
    f["arc_ver"] = d[o + 4]
    f["method"] = d[o + 5]
    f["crc32"] = u32(d, o + 6)
    f["attr"] = u32(d, o + 10)
    f["time"] = u32(d, o + 14)
    f["comp_size"] = u32(d, o + 18)
    f["orig_size"] = u32(d, o + 22)
    o += 26
    if b.flags & 0x0080:  # bit 7 -> 64 bits
        f["comp_size64"] = u32(d, o); o += 4
        f["orig_size64"] = u32(d, o); o += 4
    f["name_len"] = u16(d, o); o += 2
    f["name"] = d[o:o + f["name_len"]].decode("latin1")
    o += f["name_len"]
    f["data_off"] = o
    f["after_off"] = o + f["comp_size"]
    f["fmt"] = "2.0" if f["arc_ver"] >= 20 else "1.1"
    f["dict"] = (b.flags >> 8) & 7
    # v2.0: bit 11 (0x0800) = dictionnaire etendu: 8 Mo << (bits 8-10)
    f["dict_ext"] = bool(b.flags & 0x0800)
    return f

def dict_name(f):
    if f.get("dict_ext"):
        return "%dM(ext)" % (8 << f["dict"])
    return (DIC_SIZES_V2 if f.get("fmt") == "2.0" else DIC_SIZES)[f["dict"]]

def dump_one(path, show_data=True, extract_dir=None):
    data = open(path, "rb").read()
    print("=" * 78)
    print("%s  (%d octets)" % (path, len(data)))
    off = 0
    fmt_global = None
    while off < len(data):
        b = read_block(data, off)
        if b is None:
            print("  [%06x] IMPOSSIBLE de lire un bloc ici: %s" % (off, hexs(data[off:off + 16])))
            break
        name = BLOCK_NAMES.get(b.type, "0x%02x" % b.type)
        crcf = "" if b.crc_ok else "  *** CRC16 INVALIDE (attendu %04x) ***" % crc16_of(b.raw[2:])
        print("  [%06x] %-8s flags=0x%04x size=%d%s" % (off, name, b.flags, b.size, crcf))
        if b.type == BLOCK_MAIN:
            m = parse_main(b)
            fmt_global = m["fmt"]
            print("           -sqx- id=%s main_ver=%d vol_num=%d vol_id=0x%08x rsv=%s"
                  % (m["id"], m["arc_ver"], m["vol_num"], m["vol_id"], m["reserved"]))
            print("           flags: %s" % flagstr_main(b.flags))
        elif b.type == BLOCK_FILE:
            f = parse_filehead(b, data, b.off + 7)
            print("           name='%s' fmt=%s method=%s (raw file_ver=%d 0x%02x)"
                  % (f["name"], f["fmt"], fmt_method(f["fmt"], f["method"]), f["arc_ver"], f["arc_ver"]))
            print("           dict=%s comp=%d orig=%d attr=0x%08x time=0x%08x comp_flags=0x%02x extra=0x%04x"
                  % (dict_name(f),
                     f["comp_size"], f["orig_size"], f["attr"], f["time"],
                     f["comp_flags"], f["extra_flags"]))
            print("           flags: %s" % flagstr_file(b.flags))
            dstart = f["data_off"]
            dlen = f["comp_size"]
            if show_data and dlen:
                print("           data[%06x..%06x] %s" % (dstart, dstart + dlen - 1, hexs(data[dstart:dstart + dlen], 48)))
            if extract_dir and dlen:
                os.makedirs(extract_dir, exist_ok=True)
                out = os.path.join(extract_dir, os.path.basename(path) + "." + f["name"].replace("/", "_") + ".sqxdata")
                open(out, "wb").write(data[dstart:dstart + dlen])
                print("           -> donnees brutes ecrites dans %s" % out)
            if b.flags & 0x8000:
                print("           (bit15: blocs lies suivent)")
                off = dstart + dlen
                continue
            off = dstart + dlen
            continue
        elif b.type == BLOCK_DIR:
            p = b.payload
            n = u16(p, 4)
            print("           dir='%s'" % p[6:6 + n].decode("latin1"))
        elif b.type == BLOCK_INTERNAL:
            p = b.payload
            print("           sub_id=0x%04x vendor_id=0x%04x data=%s" % (u16(p, 0), u16(p, 2), hexs(p[4:], 24)))
        elif b.type == BLOCK_END:
            pass
        elif b.type == BLOCK_COMMENT:
            p = b.payload
            print("           ver=%d method=0x%02x comp=%d orig=%d" % (p[0], p[1], u16(p, 2), u16(p, 4)))
        off += b.size
    return

def flagstr_main(fl):
    s = []
    names = {1: "HEADERS_ENCRYPTED", 2: "SOLID", 3: "MULTIVOL", 4: "LASTVOL", 5: "COMMENT", 13: "AV"}
    for i, n in names.items():
        if fl & (1 << i): s.append(n)
    return ",".join(s) or "-"

def flagstr_file(fl):
    s = []
    names = {1: "ENCRYPTED", 2: "SOLID", 3: "CONT_FROM_PREV", 4: "CONT_ON_NEXT",
             5: "COMMENT", 6: "ASCII_NAME", 7: "SIZE64", 15: "MORE_BLOCKS"}
    for i, n in names.items():
        if fl & (1 << i): s.append(n)
    return ",".join(s) or "-"

# ---------------------------------------------------------------- summary
def summary(paths):
    print("%-34s %-4s %-6s %-16s %-5s %8s %8s %s" % ("fichier", "fmt", "method", "dict", "blk", "comp", "orig", "flags"))
    for path in paths:
        try:
            data = open(path, "rb").read()
            b = read_block(data, 0)
            if b is None or b.type != BLOCK_MAIN:
                print("%-34s PAS UN ARCHIVE SQX" % os.path.basename(path)); continue
            m = parse_main(b)
            off = b.size
            fb = None
            nblocks = 0
            while off < len(data):
                bb = read_block(data, off)
                if bb is None: break
                nblocks += 1
                if bb.type == BLOCK_FILE:
                    ff = parse_filehead(bb, data, bb.off + 7)
                    if fb is None:
                        fb = ff
                    off = ff["after_off"]
                    continue
                off += bb.size
            f = fb or {}
            print("%-34s %-4s %-6s %-16s %-5d %8s %8s %s" % (
                os.path.basename(path), f.get("fmt", "??"),
                "0x%02x" % f.get("method", -1) if fb else "-",
                dict_name(f) if fb else "-",
                nblocks, f.get("comp_size", "-"), f.get("orig_size", "-"),
                flagstr_main(b.flags)))
        except Exception as e:
            print("%-34s ERREUR %s" % (os.path.basename(path), e))

# ---------------------------------------------------------------- compat
# meme sortie que `bin/sqx list` (nom fmt method comp orig) pour les tests
# croises Python <-> C (cf. tools/test_container.sh).
def compat(paths):
    for path in paths:
        try:
            data = open(path, "rb").read()
            b = read_block(data, 0)
            if b is None or b.type != BLOCK_MAIN:
                continue
            off = b.size
            fb = None
            while off < len(data):
                bb = read_block(data, off)
                if bb is None: break
                if bb.type == BLOCK_FILE:
                    ff = parse_filehead(bb, data, bb.off + 7)
                    if fb is None: fb = ff
                    off = ff["after_off"]
                    continue
                off += bb.size
            if fb is None: continue
            print("%-34s %-4s 0x%02x %d %d" % (
                os.path.basename(path), fb["fmt"], fb["method"],
                fb["comp_size"], fb["orig_size"]))
        except Exception as e:
            print("# %s ERREUR %s" % (os.path.basename(path), e))

# ---------------------------------------------------------------- main
if __name__ == "__main__":
    args = sys.argv[1:]
    if not args:
        print(__doc__); sys.exit(1)
    if args[0] == "--summary":
        summary(args[1:]); sys.exit(0)
    if args[0] == "--compat":
        compat(args[1:]); sys.exit(0)
    if args[0] == "--extract-data":
        for p in args[2:]:
            dump_one(p, show_data=False, extract_dir=args[1])
        sys.exit(0)
    for p in args:
        dump_one(p)
