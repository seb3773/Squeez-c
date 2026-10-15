/* huff.c — Canonical Huffman tables for SQX (faithful port of ctab.c) */
#include "huff.h"
#include <stdlib.h>
#include <string.h>

const uint32_t sqx_len2_offsets[SQX_LEN2_CODES] = {0, 4, 8, 16, 32, 64, 128, 192};
const uint32_t sqx_len2_ebits[SQX_LEN2_CODES]   = {2, 2, 3, 4, 5, 6, 6, 6};

const uint32_t sqx_len3_offsets[SQX_LEN3_CODES] = {0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
const uint32_t sqx_len3_ebits[SQX_LEN3_CODES]   = {0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};

/* Format 2.0: length-3 table has a 16th code (symbol 285).
 * Offsets recovered from sqx20.dll (0x10063030 / 0x10063070). */
const uint32_t sqx_len3_offsets2[SQX_LEN3_CODES2] = {0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384};
const uint32_t sqx_len3_ebits2[SQX_LEN3_CODES2]   = {0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};

const uint32_t sqx_len_offsets[SQX_LEN_CODES] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 20, 24, 32, 40, 48, 64, 80, 96, 128, 160, 192, 224, 0};
const uint32_t sqx_len_ebits[SQX_LEN_CODES]   = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 5, 5, 5, 0};

static const uint32_t base_table_1m[SQX_DIST_CODES_1M + 1] = {
    0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768,
    1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152,
    65536, 98304, 131072, 196608, 262144, 327680, 393216, 458752, 524288, 589824,
    655360, 720896, 786432, 851968, 917504, 983040, 1048576};
static const uint32_t base_bits_1m[SQX_DIST_CODES_1M] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16};

static const uint32_t base_table_2m[SQX_DIST_CODES_2M + 1] = {
    0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768,
    1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152,
    65536, 98304, 131072, 196608, 262144, 393216, 524288, 655360, 786432, 917504,
    1048576, 1179648, 1310720, 1441792, 1572864, 1703936, 1835008, 1966080, 2097152};
static const uint32_t base_bits_2m[SQX_DIST_CODES_2M] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 17, 17,
    17, 17, 17, 17, 17, 17, 17, 17, 17, 17};

static const uint32_t base_table_4m[SQX_DIST_CODES_4M + 1] = {
    0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768,
    1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152,
    65536, 98304, 131072, 196608, 262144, 393216, 524288, 786432, 1048576, 1310720,
    1572864, 1835008, 2097152, 2359296, 2621440, 2883584, 3145728, 3407872,
    3670016, 3932160, 4194304};
static const uint32_t base_bits_4m[SQX_DIST_CODES_4M] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18,
    18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18};

/* Format 2.0 only: 8 MB dictionary (54 codes, sqx20.dll 0x10063634 / 0x10063710). */
static const uint32_t base_table_8m[SQX_DIST_CODES_8M] = {
    0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768,
    1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152,
    65536, 98304, 131072, 196608, 262144, 393216, 524288, 786432, 1048576, 1572864,
    2097152, 2621440, 3145728, 3670016, 4194304, 4718592, 5242880, 5767168,
    6291456, 6815744, 7340032, 7864320};
static const uint32_t base_bits_8m[SQX_DIST_CODES_8M] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19};

void sqx_set_base_dist_tabs(sqx_dist_tab *t, uint32_t dic_size)
{
    t->base_table = base_table_1m;
    t->base_bits = base_bits_1m;
    t->num_codes = SQX_DIST_CODES_1M;
    if (dic_size >= 8192) {
        t->base_table = base_table_8m;
        t->base_bits = base_bits_8m;
        t->num_codes = SQX_DIST_CODES_8M;
    } else if (dic_size >= 4096) {
        t->base_table = base_table_4m;
        t->base_bits = base_bits_4m;
        t->num_codes = SQX_DIST_CODES_4M;
    } else if (dic_size >= 2048) {
        t->base_table = base_table_2m;
        t->base_bits = base_bits_2m;
        t->num_codes = SQX_DIST_CODES_2M;
    }
}

int sqx_huff_alloc(sqx_huff *h, uint32_t num_code, uint32_t mb_code)
{
    h->num_code = num_code;
    h->mb_code = mb_code;
    h->lens = (uint8_t *)malloc(num_code);
    h->codes = (uint16_t *)malloc(sizeof(uint16_t) * ((size_t)1 << mb_code));
    if (!h->lens || !h->codes) {
        sqx_huff_free(h);
        return 0;
    }
    memset(h->lens, 0, num_code);
    memset(h->codes, 0, sizeof(uint16_t) * ((size_t)1 << mb_code));  /* matches SDK */
    return 1;
}

void sqx_huff_free(sqx_huff *h)
{
    free(h->lens);
    free(h->codes);
    h->lens = NULL;
    h->codes = NULL;
}

int sqx_huff_generate(sqx_huff *h)
{
    uint32_t len_count[17] = {0};
    uint32_t next_code[18] = {0};
    uint32_t ccount[17] = {0};
    uint32_t n = h->num_code, mb = h->mb_code;
    uint32_t max_tab = 1u << mb;
    const uint32_t max_len = 16;   /* HUFF_LEN = 4 bits => lengths 0..15, margin 16 */

    for (uint32_t i = 0; i < n; i++)
        len_count[h->lens[i]]++;

    next_code[1] = 0;
    for (uint32_t c = 1; c <= mb; c++)
        next_code[c + 1] = next_code[c] + (len_count[c] << (mb - c));

    if (next_code[mb + 1] != max_tab)
        return 0;                  /* Invalid Huffman tree */

    memset(h->codes, 0, sizeof(uint16_t) * max_tab);
    for (uint32_t c = 1; c <= mb; c++)
        ccount[c] = 1u << (mb - c);

    for (uint32_t sym = 0; sym < n; sym++) {
        uint32_t blen = h->lens[sym];
        uint32_t nxt;
        if (blen == 0)
            continue;
        if (blen > max_len)
            return 0;
        nxt = next_code[blen] + ccount[blen];
        if (nxt > max_tab)
            return 0;
        for (uint32_t i = next_code[blen]; i < nxt; i++)
            h->codes[i] = (uint16_t)sym;
        next_code[blen] = nxt;
    }
    return 1;
}

int sqx_read_one_tab_flat(sqx_huff *h, sqx_bits *b)
{
    uint32_t code_num, count;

    code_num = sqx_bits_get(b, 4);
    h->lens[0] = (uint8_t)code_num;
    count = 1;

    while (count < h->num_code) {
        if (sqx_bits_get(b, 1) == 0) {
            h->lens[count++] = (uint8_t)code_num;
            continue;
        }
        if (sqx_bits_get(b, 1) == 0) {
            h->lens[count++] = (uint8_t)(++code_num);
            continue;
        }
        if (sqx_bits_get(b, 1) == 1) {
            code_num = sqx_bits_get(b, 4);
            h->lens[count++] = (uint8_t)code_num;
            continue;
        }
        if (sqx_bits_get(b, 1) == 0)
            h->lens[count++] = (uint8_t)(--code_num);
        else
            h->lens[count++] = (uint8_t)(code_num += 2);
    }
    return sqx_huff_generate(h);
}

int sqx_read_one_tab_tree(sqx_huff *h, sqx_bits *b, const sqx_huff *bitct)
{
    uint32_t count = 0;
    uint32_t rep_count;

    while (count < h->num_code) {
        uint32_t code_num = sqx_huff_get(bitct, b);

        if (code_num < 16) {
            h->lens[count++] = (uint8_t)code_num;
        } else if (code_num == 16) {
            rep_count = 3 + sqx_bits_get(b, 2);
            while (count < h->num_code && rep_count-- > 0)
                h->lens[count] = h->lens[count - 1], count++;
        } else {
            if (code_num == 17)
                rep_count = 3 + sqx_bits_get(b, 3);
            else
                rep_count = 11 + sqx_bits_get(b, 7);
            while (count < h->num_code && rep_count-- > 0)
                h->lens[count++] = 0;
        }
    }
    return sqx_huff_generate(h);
}
