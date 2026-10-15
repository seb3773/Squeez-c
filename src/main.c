/* main.c — SQX command-line utility (decoder / encoder).
 *
 *   sqx info    archive.sqx ...     dump blocks + headers + CRC
 *   sqx list    archive.sqx ...     list archive contents (like `sqc l`)
 *   sqx test    archive.sqx ...     verify block CRCs (container level)
 *   sqx raw     outdir archive...   extract raw compressed data streams (.sqxdata)
 *   sqx extract outdir archive...   decode and write files (CRC32 validation)
 */
#include "sqx.h"
#include "audio.h"
#include "create.h"
#include "enc.h"
#include "lz.h"
#include "ppm8.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define sqx_mkdir(p) _mkdir(p)
#else
#define sqx_mkdir(p) mkdir(p, 0777)
#endif

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (uint8_t *)malloc((size_t)n + 16);
    if (!buf) { fclose(f); return NULL; }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    memset(buf + n, 0, 16);          /* margin: bit reader fetches 32-bit words */
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void print_hex(const uint8_t *p, size_t n, size_t max)
{
    size_t i, lim = n < max ? n : max;
    for (i = 0; i < lim; i++)
        printf("%02x%s", p[i], (i + 1 < lim) ? " " : "");
    if (n > lim) printf(" ...");
}

/* Searches for the beginning of an SQX archive in a file (SFX executables). */
static size_t find_archive_start(const uint8_t *buf, size_t len)
{
    size_t o;
    for (o = 0; o + 25 <= len; o++) {
        if (buf[o + 2] != SQX_BLOCK_MAIN)
            continue;
        if (memcmp(buf + o + 7, "-sqx-", 5) != 0)
            continue;
        if (buf[o + 5] < 25 || buf[o + 6] != 0)
            continue;
        if (sqx_crc16(buf + o + 2, (size_t)buf[o + 5] - 2) !=
            (uint16_t)(buf[o] | (buf[o + 1] << 8)))
            continue;
        return o;
    }
    return (size_t)-1;
}

/* Creates parent directories for a file path */
static void mkdirs_for(const char *filepath)
{
    char tmp[4096];
    size_t n = strlen(filepath);
    if (n >= sizeof(tmp)) return;
    memcpy(tmp, filepath, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            sqx_mkdir(tmp);
            tmp[i] = '/';
        }
    }
}

/* Validates relative path safety against path traversal (Zip-Slip) attacks */
static int is_safe_relpath(const char *path)
{
    if (!path || !path[0])
        return 0;
    if (path[0] == '/' || path[0] == '\\')
        return 0;
    if (isalpha((unsigned char)path[0]) && path[1] == ':')
        return 0;

    int depth = 0;
    const char *p = path;
    while (*p) {
        while (*p == '/' || *p == '\\')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != '/' && *p != '\\')
            p++;
        size_t len = (size_t)(p - start);
        if (len == 1 && start[0] == '.')
            continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            depth--;
            if (depth < 0)
                return 0;
        } else {
            depth++;
        }
    }
    return 1;
}

/* Formats paths: normalizes backslashes to slashes; if opt_unix is set,
 * forces lowercase and strips leading slashes or relative prefixes. */
static void format_path(char *dst, const char *src, size_t dst_max, int opt_unix)
{
    size_t i = 0;
    if (opt_unix) {
        while (*src == '/' || *src == '\\')
            src++;
        while (src[0] == '.' && (src[1] == '/' || src[1] == '\\'))
            src += 2;
    }
    while (*src && i + 1 < dst_max) {
        char c = *src++;
        if (c == '\\')
            c = '/';
        if (opt_unix)
            c = (char)tolower((unsigned char)c);
        dst[i++] = c;
    }
    dst[i] = '\0';
}


/*
 * repack: decodes each LZ stream in the archive, then re-encodes it from IR.
 * If the bit-layer (tables, codes, headers) is exact, the output matches
 * byte-for-byte: this is the bit-identity test of the encoder.
 */
static int repack(const char *inpath, const char *outpath, int *repacked,
                  int *diffbytes)
{
    size_t len = 0, off = 0;
    uint8_t *buf = read_file(inpath, &len);
    sqx_lz lz;
    int bad = 0;

    if (!buf) { fprintf(stderr, "%s: cannot read file\n", inpath); return 1; }
    if (memcmp(buf + 7, "-sqx-", 5) != 0) {
        fprintf(stderr, "%s: not an SQX archive\n", inpath);
        free(buf);
        return 1;
    }
    if (!sqx_lz_alloc(&lz)) { free(buf); return 1; }

    while (off + 7 <= len) {
        sqx_block b;
        if (!sqx_block_read(buf + off, len - off, &b))
            break;
        b.offset = off;
        if (b.type != SQX_BLOCK_FILE) {
            off = (b.type == SQX_BLOCK_MAIN || b.type == SQX_BLOCK_DIR) ? off + b.size : off + b.size;
            continue;
        }
        {
            sqx_file f;
            if (!sqx_parse_file(&b, buf, len, &f)) { bad++; break; }
            if (f.method >= SQX_METHOD_NORMAL && f.method <= SQX_METHOD_BEST) {
                sqx_ir ir;
                sqx_bitw bw;
                uint32_t dic = (uint32_t)sqx_dict_size(b.flags, f.arcer_ver);
                int solid = (b.flags & 0x0004) ? 1 : 0;
                const uint8_t *data = buf + f.data_offset;

                if (!sqx_lz_decode_ir(&lz, data, (size_t)f.comp_size, f.orig_size,
                                      solid, dic, f.comp_flags, f.arcer_ver, &ir)) {
                    printf("  %.*s: decompression failed\n", f.name_len, f.name);
                    bad++;
                } else {
                    if (getenv("SQX_ENC_TRACE"))
                        printf("  %.*s: %zu block(s), %zu op(s)\n",
                               f.name_len, f.name, ir.num_blocks, ir.num_ops);
                    sqx_bitw_init(&bw, (size_t)f.comp_size + 64);
                    if (!sqx_enc_ir(&ir, data, (size_t)f.comp_size, &bw)) {
                        printf("  %.*s: encoding failed\n", f.name_len, f.name);
                        bad++;
                    } else {
                        size_t d = 0;
                        if (bw.len != (size_t)f.comp_size) {
                            printf("  %.*s: re-encoded size %zu (bits=%llu, ops=%lu) != %llu\n",
                                   f.name_len, f.name, bw.len,
                                   (unsigned long long)sqx_bitw_bits(&bw),
                                   sqx_enc_ops_written,
                                   (unsigned long long)f.comp_size);
                            bad++;
                        } else {
                            for (; d < bw.len && buf[f.data_offset + d] == bw.buf[d]; d++)
                                ;
                            if (d != bw.len) {
                                printf("  %.*s: 1st difference at offset %zu "
                                       "(byte 0x%02x vs 0x%02x)\n",
                                       f.name_len, f.name, d,
                                       buf[f.data_offset + d], bw.buf[d]);
                                *diffbytes += 1;
                                bad++;
                            } else {
                                (*repacked)++;
                            }
                            memcpy(buf + f.data_offset, bw.buf, bw.len);
                        }
                    }
                    sqx_bitw_free(&bw);
                    sqx_ir_free(&ir);
                }
            }
            off = f.next_offset;
            continue;
        }
    }

    sqx_lz_free(&lz);
    if (outpath) {
        FILE *g = fopen(outpath, "wb");
        if (!g || (len && fwrite(buf, 1, len, g) != len)) {
            fprintf(stderr, "%s: cannot write file\n", outpath);
            if (g) fclose(g);
            free(buf);
            return 1;
        }
        fclose(g);
    }
    free(buf);
    return bad;
}

static int walk(const char *path, uint8_t *buf, size_t len, int mode, const char *outdir,
                int opt_unix, int opt_bare)
{
    size_t off = 0;
    sqx_main main_hdr = {0};
    int nblocks = 0, bad = 0, fmt = 0;
    sqx_lz lz;
    sqx_audio audio;
    sqx_ppm8 *p8 = NULL;
    int lz_ready = 0;
    int audio_ready = 0;
    int errors = 0, extracted = 0;

    if (len >= 7 && !(buf[2] == SQX_BLOCK_MAIN && len >= 12 && memcmp(buf + 7, "-sqx-", 5) == 0)) {
        size_t start = find_archive_start(buf, len);
        if (start != (size_t)-1) {
            if (mode == 0)
                printf("  (SQX archive found at offset %zu: SFX/executable stub)\n", start);
            off = start;
        }
    }

    if ((mode == 4 || mode == 3) && !sqx_lz_alloc(&lz)) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    if (mode == 4 || mode == 3) lz_ready = 1;
    if (mode == 4) {
        sqx_audio_alloc(&audio);
        audio_ready = 1;
        p8 = sqx_ppm8_new();
    }

    for (;;) {
        sqx_block b;
        if (off >= len) break;
        if (!sqx_block_read(buf + off, len - off, &b)) {
            printf("  [%06zx] !! unreadable block (offset %zu / %zu)\n", off, off, len);
            bad++;
            break;
        }
        b.offset = off;
        nblocks++;
        if (!sqx_block_check_crc(&b)) bad++;

        if (mode == 0) {
            printf("  [%06zx] %-8s flags=0x%04x size=%d%s\n",
                   off, sqx_block_name(b.type), b.flags, b.size,
                   sqx_block_check_crc(&b) ? "" : "   *** CRC16 INVALID ***");
        }

        if (b.type == SQX_BLOCK_MAIN) {
            if (sqx_parse_main(&b, &main_hdr)) {
                if (mode == 0)
                    printf("           -sqx- main_ver=11 vol_num=%u vol_id=0x%08x flags=0x%04x\n",
                           main_hdr.vol_num, main_hdr.vol_id, b.flags);
            }
            off += b.size;
            continue;
        }

        if (b.type == SQX_BLOCK_FILE) {
            sqx_file f;
            if (!sqx_parse_file(&b, buf, len, &f)) {
                printf("  [%06zx] unreadable FILE header\n", off);
                bad++;
                break;
            }
            fmt = f.arcer_ver;

            if (mode == 0) {
                printf("           name='%.*s' fmt=%s method=0x%02x (%s)\n",
                       f.name_len, f.name, f.arcer_ver >= 20 ? "2.0" : "1.1",
                       f.method, sqx_method_name(f.method, f.arcer_ver));
                printf("           dict=%s comp=%llu orig=%llu crc32=0x%08x attr=0x%08x\n",
                       sqx_dict_name(b.flags, f.arcer_ver),
                       (unsigned long long)f.comp_size, (unsigned long long)f.orig_size,
                       f.crc32, f.attrs);
                if (f.comp_size) {
                    printf("           data[%06zx..%06zx] ",
                           f.data_offset, f.data_offset + (size_t)f.comp_size - 1);
                    print_hex(buf + f.data_offset, (size_t)f.comp_size, 48);
                    printf("\n");
                }
            } else if (mode == 1) {
                char fname[4096];
                char raw[4096];
                size_t nl = f.name_len < sizeof(raw) - 1 ? f.name_len : sizeof(raw) - 1;
                memcpy(raw, f.name, nl);
                raw[nl] = '\0';
                format_path(fname, raw, sizeof(fname), opt_unix);
                if (opt_bare) {
                    printf("%s\n", fname);
                } else {
                    printf("%-40s %10llu  %s\n", fname,
                           (unsigned long long)f.orig_size,
                           sqx_method_name(f.method, f.arcer_ver));
                }
            } else if (mode == 3) {
                char out[4200];
                snprintf(out, sizeof(out), "%s/%s.%s.sqxdata", outdir,
                         strrchr(path, '/') ? strrchr(path, '/') + 1 : path, "raw");
                FILE *g = fopen(out, "wb");
                if (g) {
                    size_t w = fwrite(buf + f.data_offset, 1, (size_t)f.comp_size, g);
                    fclose(g);
                    if (w != (size_t)f.comp_size) { printf("  failed to write %s\n", out); bad++; }
                } else { printf("  cannot create %s\n", out); bad++; }
            } else if (mode == 4) {
                /* decoding */
                const uint8_t *data = buf + f.data_offset;
                size_t dlen = (size_t)f.comp_size;
                const uint8_t *content = NULL;
                uint8_t *pmbuf = NULL;
                size_t clen = 0;
                int ok = 0;
                char raw_name[4096];
                char name[4096];
                size_t nl = f.name_len < sizeof(raw_name) - 1 ? f.name_len : sizeof(raw_name) - 1;

                memcpy(raw_name, f.name, nl);
                raw_name[nl] = '\0';

                if (!is_safe_relpath(raw_name)) {
                    fprintf(stderr, "Security warning: skipping unsafe path traversal '%s'\n", raw_name);
                    errors++;
                    off = f.next_offset;
                    continue;
                }
                format_path(name, raw_name, sizeof(name), opt_unix);

                if (f.method == SQX_METHOD_STORED) {
                    content = data;
                    clen = dlen;
                    ok = 1;
                } else if (f.method >= SQX_METHOD_NORMAL && f.method <= SQX_METHOD_BEST) {
                    uint32_t dic = (uint32_t)sqx_dict_size(b.flags, f.arcer_ver);
                    int solid = (b.flags & 0x0004) ? 1 : 0;
                    ok = sqx_lz_decode(&lz, data, dlen, f.orig_size, solid, dic, f.comp_flags, f.arcer_ver);
                    if (ok) {
                        content = lz.out;
                        clen = lz.out_len;
                    }
                } else if (f.method == SQX_METHOD_TEXT && f.arcer_ver >= SQX_ARCVER_20
                           && p8) {
                    /* PPMd var. I (SQX 2.0 method 6) */
                    if (f.orig_size <= 0x40000000u) {
                        pmbuf = (uint8_t *)malloc((size_t)f.orig_size + 1);
                        if (pmbuf &&
                            sqx_ppm8_decode(p8, data, dlen, f.orig_size, pmbuf,
                                            (size_t)f.orig_size + 1)) {
                            ok = 1;
                            content = pmbuf;
                            clen = (size_t)f.orig_size;
                        }
                    }
                } else if (f.method == SQX_METHOD_AUDIO) {
                    ok = sqx_audio_decode(&audio, data, dlen, f.orig_size, f.arcer_ver);
                    if (ok) {
                        content = audio.out;
                        clen = audio.out_len;
                    }
                } else {
                    printf("%s: method 0x%02x (%s) not supported\n", name, f.method,
                           sqx_method_name(f.method, f.arcer_ver));
                }

                if (ok && clen == f.orig_size && (lz_ready || audio_ready)) {
                    /* audio method includes its internal header in CRC calculation (cx_audio.c) */
                    uint32_t got = (f.method == SQX_METHOD_AUDIO)
                                       ? audio.crc
                                       : sqx_crc32_raw(SQX_CRC32_INIT, content, clen);
                    if (got != f.crc32) {
                        printf("%s: CRC32 0x%08x expected 0x%08x FAILED\n", name, got, f.crc32);
                        errors++;
                        if (getenv("SQX_FORCE_WRITE")) {
                            char dbgp[4200];
                            snprintf(dbgp, sizeof(dbgp), "%s/%s.crc-bad", outdir, name);
                            mkdirs_for(dbgp);
                            FILE *g = fopen(dbgp, "wb");
                            if (g) {
                                fwrite(content, 1, clen, g);
                                fclose(g);
                                printf("  (content written to %s, %zu bytes)\n",
                                       dbgp, clen);
                            }
                        }
                    } else {
                        char outp[4200];
                        snprintf(outp, sizeof(outp), "%s/%s", outdir, name);
                        mkdirs_for(outp);
                        FILE *g = fopen(outp, "wb");
                        if (!g) {
                            printf("%s: cannot create file (%s)\n", outp, strerror(errno));
                            errors++;
                        } else {
                            if (clen && fwrite(content, 1, clen, g) != clen) {
                                printf("%s: write failed\n", outp);
                                errors++;
                            } else {
                                extracted++;
                                if (opt_bare) {
                                    printf("%s\n", name);
                                } else {
                                    printf("%s (%zu bytes, CRC OK, %s)\n", name, clen,
                                           sqx_method_name(f.method, f.arcer_ver));
                                }
                            }
                            fclose(g);
                        }
                    }
                } else if (ok) {
                    printf("%s: decoded size %zu != %llu expected\n", name, clen,
                           (unsigned long long)f.orig_size);
                    errors++;
                } else if (f.method >= SQX_METHOD_NORMAL && f.method <= SQX_METHOD_BEST) {
                    printf("%s: decompression failed\n", name);
                    errors++;
                } else if (f.method == SQX_METHOD_TEXT && f.arcer_ver >= SQX_ARCVER_20) {
                    printf("%s: PPMd (method 6) decompression failed\n", name);
                    errors++;
                } else if (f.method == SQX_METHOD_AUDIO) {
                    /* Both 2.0 codecs (8 and 16-bit) are supported; this
                     * check only guards mode 0 predictors 5..7, which the
                     * official DLL never emits (§07-audio.md). */
                    if (f.arcer_ver >= 20 && audio.unsupported)
                        printf("%s: unsupported audio 2.0 codec variant"
                               " (known limitation)\n", name);
                    else
                        printf("%s: audio decompression failed\n", name);
                    errors++;
                }
                free(pmbuf);
            }
            off = f.next_offset;
            continue;
        }

        if (b.type == SQX_BLOCK_DIR) {
            const uint8_t *p = b.raw + SQX_BLOCK_HDR_SIZE;
            size_t plen = (size_t)b.size - SQX_BLOCK_HDR_SIZE;
            if (plen >= 6) {
                uint16_t dl = (uint16_t)(p[4] | (p[5] << 8));
                if ((size_t)dl <= plen - 6) {
                    char raw_dir[4096], dname[4096];
                    size_t dlen = dl < sizeof(raw_dir) - 1 ? dl : sizeof(raw_dir) - 1;
                    memcpy(raw_dir, p + 6, dlen);
                    raw_dir[dlen] = '\0';
                    format_path(dname, raw_dir, sizeof(dname), opt_unix);

                    if (mode == 0)
                        printf("           dir='%s'\n", dname);
                    else if (mode == 1) {
                        if (opt_bare) {
                            size_t sl = strlen(dname);
                            if (sl && dname[sl - 1] != '/')
                                printf("%s/\n", dname);
                            else
                                printf("%s\n", dname);
                        } else {
                            printf("%-40s %10s  (dir)\n", dname, "-");
                        }
                    } else if (mode == 4) {
                        char dirpath[4200];
                        if (!is_safe_relpath(raw_dir)) {
                            fprintf(stderr, "Security warning: skipping unsafe path traversal in directory '%s'\n", raw_dir);
                            errors++;
                        } else {
                            snprintf(dirpath, sizeof(dirpath), "%s/%s", outdir, dname);
                            mkdirs_for(dirpath);
                            sqx_mkdir(dirpath);
                        }
                    }
                }
            }
            off += b.size;
            continue;
        }

        off += b.size;
    }

    if (lz_ready)
        sqx_lz_free(&lz);
    if (audio_ready)
        sqx_audio_free(&audio);
    sqx_ppm8_free(p8);

    if (!opt_bare) {
        if (mode == 0 || mode == 2)
            printf("  -> format=%s, %d blocks, %s\n",
                   fmt >= 20 ? "2.0" : "1.1", nblocks,
                   bad ? "CRC/STRUCTURE ERRORS" : "CRC OK");
        if (mode == 4)
            printf("  -> %d file(s) extracted, %d error(s)\n", extracted, errors);
    }
    return bad + errors;
}

int main(int argc, char **argv)
{
    const char *cmd = NULL;
    int mode, i, bad = 0;
    int opt_unix = 0;
    int opt_bare = 0;
    char *pos_args[512];
    int n_pos = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--unix") == 0) {
            opt_unix = 1;
        } else if (strcmp(argv[i], "-1") == 0 || strcmp(argv[i], "--bare") == 0) {
            opt_bare = 1;
        } else if (strcmp(argv[i], "-1u") == 0 || strcmp(argv[i], "-u1") == 0) {
            opt_unix = 1;
            opt_bare = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            if (!opt_bare) {
                printf("Squeez (c) 1993-2008 Sven Ritter\n"
                       "pure C reimplementation by seb3773\n"
                       "https://github.com/seb3773/Squeez-c\n\n");
            }
            printf("usage: %s [options] <command> archive.sqx ...\n"
                   "       %s create out.sqx [--fmt=11|20] [--method=1..6] [--dict=KB] [--ppm-order=2..16] [--ppm-mem=MB] file...\n\n"
                   "commands:\n"
                   "  list, l     List archive contents\n"
                   "  extract, x  Extract files with folder structure\n"
                   "  info, i     Dump blocks and header structures\n"
                   "  test, t     Verify block checksums (CRC16)\n"
                   "  raw         Extract raw compressed data streams (.sqxdata)\n"
                   "  create, a   Create new SQX archive\n"
                   "  check       Verify encoder bit-identity against packed archives\n"
                   "  repack      Re-encode archive streams from IR\n\n"
                   "options:\n"
                   "  -u, --unix  Format paths as Unix relative paths (/ slashes, lowercase)\n"
                   "  -1, --bare  Bare output format (one path per line, ideal for scripts, xargs, grep)\n"
                   "  -h, --help  Show this help message\n",
                   argv[0], argv[0]);
            return 0;
        } else {
            if (n_pos < 510)
                pos_args[n_pos++] = argv[i];
        }
    }

    if (!opt_bare) {
        printf("Squeez (c) 1993-2008 Sven Ritter\n"
               "pure C reimplementation by seb3773\n"
               "https://github.com/seb3773/Squeez-c\n\n");
    }

    if (n_pos < 1) {
        printf("usage: %s [options] <command> archive.sqx ...\n"
               "       %s create out.sqx [--fmt=11|20] [--method=1..6] [--dict=KB] [--ppm-order=2..16] [--ppm-mem=MB] file...\n\n"
               "commands:\n"
               "  list, l     List archive contents\n"
               "  extract, x  Extract files with folder structure\n"
               "  info, i     Dump blocks and header structures\n"
               "  test, t     Verify block checksums (CRC16)\n"
               "  raw         Extract raw compressed data streams (.sqxdata)\n"
               "  create, a   Create new SQX archive\n"
               "  check       Verify encoder bit-identity against packed archives\n"
               "  repack      Re-encode archive streams from IR\n\n"
               "options:\n"
               "  -u, --unix  Format paths as Unix relative paths (/ slashes, lowercase)\n"
               "  -1, --bare  Bare output format (one path per line, ideal for scripts, xargs, grep)\n"
               "  -h, --help  Show this help message\n",
               argv[0], argv[0]);
        return 2;
    }

    cmd = pos_args[0];
    if (strcmp(cmd, "create") == 0 || strcmp(cmd, "a") == 0) {
        int arcer_ver = 11, method = 4;
        uint32_t dic_kb = 4096;
        unsigned ppm_order = 0, ppm_mem = 0;
        char **files = NULL;
        int n_files = 0, k;

        if (n_pos < 3) {
            fprintf(stderr,
                    "usage: %s create out.sqx [--fmt=11|20] [--method=1..6]"
                    " [--dict=KB] [--ppm-order=2..16] [--ppm-mem=MB] file...\n"
                    "       (method 5 = audio 2.0 from 16-bit PCM WAV, 6 = PPMd)\n",
                    argv[0]);
            return 2;
        }
        files = (char **)calloc((size_t)n_pos, sizeof(char *));
        if (!files)
            return 1;
        for (k = 2; k < n_pos; k++) {
            if (strncmp(pos_args[k], "--fmt=", 6) == 0)
                arcer_ver = atoi(pos_args[k] + 6);
            else if (strncmp(pos_args[k], "--method=", 9) == 0)
                method = atoi(pos_args[k] + 9);
            else if (strncmp(pos_args[k], "--dict=", 7) == 0)
                dic_kb = (uint32_t)atoi(pos_args[k] + 7);
            else if (strncmp(pos_args[k], "--ppm-order=", 12) == 0)
                ppm_order = (unsigned)atoi(pos_args[k] + 12);
            else if (strncmp(pos_args[k], "--ppm-mem=", 10) == 0)
                ppm_mem = (unsigned)atoi(pos_args[k] + 10);
            else
                files[n_files++] = pos_args[k];
        }
        if (arcer_ver != 11 && arcer_ver != 20) {
            fprintf(stderr, "--fmt must be 11 or 20\n");
            free(files);
            return 2;
        }
        if (method < 1 || method > 6) {
            fprintf(stderr, "--method must be 1 to 4 (LZH), 5 (audio 2.0)"
                            " or 6 (PPMd 2.0)\n");
            free(files);
            return 2;
        }
        if (method == 6 && arcer_ver != 20) {
            fprintf(stderr, "Method 6 (PPMd) is only supported in SQX 2.0 format\n");
            free(files);
            return 2;
        }
        if (method == 5 && arcer_ver != 20) {
            fprintf(stderr, "Audio encoding is only supported in SQX 2.0 format"
                            " (16-bit PCM WAV)\n");
            free(files);
            return 2;
        }
        if (n_files == 0) {
            fprintf(stderr, "create: no input files\n");
            free(files);
            return 2;
        }
        bad = sqx_create_archive(pos_args[1], files, n_files, arcer_ver, method,
                                 dic_kb * 1024u, ppm_order, ppm_mem);
        if (!bad) {
            if (method == 6)
                printf("%s: %d file(s) compressed (fmt %d, method 6 PPMd"
                       " order %u, memory %u MB, dict %u KB)\n",
                       pos_args[1], n_files, arcer_ver, ppm_order ? ppm_order : 6,
                       ppm_mem ? ppm_mem : 32, dic_kb);
            else if (method == 5)
                printf("%s: %d file(s) compressed (fmt %d, method 5 audio"
                       " 2.0 16-bit)\n", pos_args[1], n_files, arcer_ver);
            else
                printf("%s: %d file(s) compressed (fmt %d, method %d, dict %u KB)\n",
                       pos_args[1], n_files, arcer_ver, method, dic_kb);
        }
        free(files);
        return bad ? 1 : 0;
    }
    if (strcmp(cmd, "repack") == 0 || strcmp(cmd, "check") == 0) {
        int repacked = 0, diff = 0;
        int rc = 0;
        int write_out = strcmp(cmd, "repack") == 0;
        int first = write_out ? 2 : 1;

        if (n_pos < first + 1) {
            fprintf(stderr, "usage: %s %s %sarchive.sqx ...\n",
                    argv[0], cmd, write_out ? "out.sqx " : "");
            return 2;
        }
        for (i = first; i < n_pos; i++)
            rc += repack(pos_args[i], write_out ? pos_args[1] : NULL, &repacked, &diff);
        printf("repack: %d re-encoded streams identical, %d divergence(s), %d error(s)\n",
               repacked, diff, rc);
        return rc ? 1 : 0;
    }
    mode = (strcmp(cmd, "info") == 0 || strcmp(cmd, "i") == 0) ? 0 :
           (strcmp(cmd, "list") == 0 || strcmp(cmd, "l") == 0) ? 1 :
           (strcmp(cmd, "test") == 0 || strcmp(cmd, "t") == 0) ? 2 :
           (strcmp(cmd, "raw") == 0) ? 3 :
           (strcmp(cmd, "extract") == 0 || strcmp(cmd, "x") == 0 || strcmp(cmd, "e") == 0) ? 4 : -1;
    if (mode < 0) { fprintf(stderr, "unknown command: %s\n", cmd); return 1; }

    int first = (mode == 3 || mode == 4) ? 2 : 1;
    if ((mode == 3 || mode == 4) && n_pos < 3) {
        fprintf(stderr, "%s: missing output directory\n", cmd);
        return 2;
    }
    if (mode < 3 && n_pos < 2) {
        fprintf(stderr, "%s: missing archive\n", cmd);
        return 2;
    }

    for (i = first; i < n_pos; i++) {
        size_t len = 0;
        uint8_t *buf = read_file(pos_args[i], &len);
        if (!buf) { fprintf(stderr, "%s: cannot read file\n", pos_args[i]); bad++; continue; }
        if (mode == 0 && !opt_bare)
            printf("=== %s (%zu bytes)\n", pos_args[i], len);
        bad += walk(pos_args[i], buf, len, mode,
                    (mode == 3 || mode == 4) ? pos_args[1] : NULL,
                    opt_unix, opt_bare);
        free(buf);
    }
    return bad ? 1 : 0;
}
