/* huff.h — Canonical Huffman tables for SQX (port of ctab.c from SDK).
 *
 * Each Huffman decoder table is a flat array of 1 << mb_code entries
 * indexed by the mb_code leading bits of the stream.
 */
#ifndef SQX_HUFF_H
#define SQX_HUFF_H

#include "bitio.h"
#include <stdint.h>

/* Constants from ctab.h */
#define SQX_BCODE_BITS   7
#define SQX_BCODES       19
#define SQX_LCODE_BITS   15

#define SQX_LIT_CODES    256
#define SQX_DL_CODES     1
#define SQX_REP_CODES    4
#define SQX_LEN2_CODES   8
#define SQX_LEN3_CODES   15
#define SQX_LEN_CODES    (24 + 1)

#define SQX_DIST_CODES_1M 48
#define SQX_DIST_CODES_2M 50
#define SQX_DIST_CODES_4M 52
#define SQX_DIST_CODES_8M 54   /* format 2.0 only */
#define SQX_DIST_CODES    56

#define SQX_LEN2_START (SQX_LIT_CODES + SQX_DL_CODES + SQX_REP_CODES)
#define SQX_LEN3_START (SQX_LEN2_START + SQX_LEN2_CODES)
#define SQX_LEN_START  (SQX_LEN3_START + SQX_LEN3_CODES)
#define SQX_NC         (SQX_LIT_CODES + SQX_DL_CODES + SQX_REP_CODES + \
                        SQX_LEN2_CODES + SQX_LEN3_CODES + SQX_LEN_CODES + 1)

/* --- Format 2.0 (disassembled from sqx20.dll + byte-by-byte verified)
 *
 * The 2.0 symbol alphabet extends 1.1 with two additional codes, shifting groups:
 *   0..255     = literals
 *   256        = repeat last (len, dist)
 *   257        = copy ONE byte at "current distance" (see lz.c)
 *   258..261   = repeated match distances k=1..4 (1.1: 257..260)
 *   262..269   = matches of length 2             (1.1: 261..268)
 *   270..285   = matches of length 3             (1.1: 269..283)
 *                (16th code, 285, has base offset 16384 and 14 extra bits)
 *   286..309   = matches of length >= 4          (1.1: 284..307)
 *   310        = long length 14 bits             (1.1: NC-2 = 308)
 * The litlen table for 2.0 has 311 codes (instead of 310) and the distance
 * table has 54 codes (instead of 56). */
#define SQX_NC2           311u   /* litlen table size for 2.0 */
#define SQX_LEN2_START2   262u
#define SQX_LEN3_START2   270u
#define SQX_LEN_START2    286u
#define SQX_SYM_COPY1     257u   /* copy 1 byte at current distance */
#define SQX_SYM_LEN3_16   285u   /* 16th length-3 code (base 16384) */
#define SQX_DIST_CODES2   54u
#define SQX_DIST_LOW16    16u   /* table for lower 4 bits of distance */
#define SQX_LEN3_CODES2   16u   /* length-3 codes in 2.0 */

typedef struct {
    uint32_t num_code;
    uint32_t mb_code;      /* Index bit count for flat lookup table */
    uint16_t *codes;       /* 1 << mb_code entries */
    uint8_t  *lens;        /* num_code entries */
} sqx_huff;

int  sqx_huff_alloc(sqx_huff *h, uint32_t num_code, uint32_t mb_code);
void sqx_huff_free(sqx_huff *h);

/* Build decoding table from code lengths (GenerateUCTab) */
int  sqx_huff_generate(sqx_huff *h);

/* Peek next symbol without consuming bits */
static inline uint16_t sqx_huff_peek(const sqx_huff *h, const sqx_bits *b)
{
    return h->codes[sqx_bits_peek(b, h->mb_code)];
}

/* Read next symbol (consumes its bits) */
static inline uint32_t sqx_huff_get(const sqx_huff *h, sqx_bits *b)
{
    uint32_t s = h->codes[sqx_bits_peek(b, h->mb_code)];
    sqx_bits_dump(b, h->lens[s]);
    return s;
}

int sqx_read_one_tab_flat(sqx_huff *h, sqx_bits *b);
int sqx_read_one_tab_tree(sqx_huff *h, sqx_bits *b, const sqx_huff *bitct);

/* Offset & extra-bit tables for length/distance (ctab.c) */
extern const uint32_t sqx_len2_offsets[SQX_LEN2_CODES];
extern const uint32_t sqx_len2_ebits[SQX_LEN2_CODES];
extern const uint32_t sqx_len3_offsets[SQX_LEN3_CODES];
extern const uint32_t sqx_len3_ebits[SQX_LEN3_CODES];
/* Format 2.0 tables: 16 length-3 codes (see huff.c) */
extern const uint32_t sqx_len3_offsets2[SQX_LEN3_CODES2];
extern const uint32_t sqx_len3_ebits2[SQX_LEN3_CODES2];
extern const uint32_t sqx_len_offsets[SQX_LEN_CODES];
extern const uint32_t sqx_len_ebits[SQX_LEN_CODES];

typedef struct {
    const uint32_t *base_table;   /* CUR_DIST_CODES + 1 entries */
    const uint32_t *base_bits;
    uint32_t num_codes;           /* CUR_DIST_CODES */
} sqx_dist_tab;

void sqx_set_base_dist_tabs(sqx_dist_tab *t, uint32_t dic_size);

#endif /* SQX_HUFF_H */
