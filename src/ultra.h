/* ultra.h — SQX Method 7 "Ultra" (LZ77 + probabilistic range coder, SQX 2.0).
 *
 * Reverse-engineered from UnSqueez.exe (Sven Ritter) Hex-Rays decompilation
 * (UnSqueez.c) and IDA listing (UnSqueez.lst):
 *   - main loop sub_40D8B1, model setup sub_40D03E, reset sub_40CF40
 *   - binary models sub_4097D1/sub_4097FA/sub_4097C2
 *   - Fenwick models sub_409FED/sub_40A05E/sub_409F18/sub_409EE0/sub_409F67
 *   - literal nibble pairs sub_409924/sub_4099AB/sub_40990C
 *   - range core sub_409BA3/sub_409C2A/sub_409C92 (TOP=0x1000000, BOT=0x10000)
 *   - tables dword_420B70/420C38 (distance base/ebits, 50 entries),
 *     dword_420D00 (length base/next/alphabet/incr/max, 9 groups).
 */
#ifndef SQX_ULTRA_H
#define SQX_ULTRA_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t nsym, incr, maxf, tot;
    uint32_t *freq;
} ultra_fm;

typedef struct {
    uint32_t f0, f1, tot, incr, maxf;
} ultra_bm;

typedef struct {
    ultra_fm sub[17];
} ultra_lit;

typedef struct {
    /* persistent models (solid archives keep these across files) */
    ultra_lit lit[2][256];
    ultra_bm  is_match[4][4][4];
    ultra_bm  is_rep[4][4];
    ultra_fm  len_dist[4];       /* alphabet 50 */
    ultra_fm  dist_low;          /* alphabet 16 */
    ultra_fm  len_new[4];        /* alphabet 9 */
    ultra_fm  len_sub[4];        /* alphabet 9 */
    ultra_fm  len_rep[4];        /* alphabet 6 */
    ultra_fm  sec_n[4][9];       /* secondary, indexed by length group */
    ultra_fm  sec_r[4][9];
    int       models_ready;

    /* dictionary window (reallocated per dic_size) */
    uint8_t  *window;
    uint32_t  dic_size;
    uint32_t  curr_pos;

    /* range coder registers */
    uint32_t  rc_low, rc_range, rc_code;

    /* output */
    uint8_t  *out;
    size_t    out_len, out_cap;
    uint32_t  file_crc;
} sqx_ultra;

int  sqx_ultra_alloc(sqx_ultra *u);
void sqx_ultra_free(sqx_ultra *u);

/* Mirrors sqx_lz_decode() semantics. Returns 1 on success. */
int sqx_ultra_decode(sqx_ultra *u, const uint8_t *src, size_t src_len,
                     uint64_t orig_size, int solid, uint32_t dic_size,
                     uint8_t comp_flags);

#endif
