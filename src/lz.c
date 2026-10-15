/* lz.c — SQX LZ77 + Huffman decompressor v1.1 and v2.0 (port of uarde.c /
 * ctab.c / arcio.c / mm_coder.c / t_coder.c from SQX SDK 2.06).
 *
 * The stream is read in 32-bit words (bitio.h); Huffman tables are reloaded
 * at each block boundary; multimedia blocks utilize the range coder.
 */
#include "lz.h"
#include "sqx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Debug tracing: SQX_TRACE=1 (consumed bits, blocks, Huffman tables) */
static int sqx_trace = -1;
static int trace_on(void)
{
    if (sqx_trace < 0)
        sqx_trace = getenv("SQX_TRACE") != NULL;
    return sqx_trace;
}
static void trace_lens(const char *name, const sqx_huff *h, int n)
{
    if (!trace_on())
        return;
    printf("[trace] lens(%s) [0..%d]:", name, n - 1);
    for (int i = 0; i < n && i < (int)h->num_code; i++)
        printf(" %d", h->lens[i]);
    printf("\n");
}

/* ----------------------------------------------------------- Allocation */

int sqx_lz_alloc(sqx_lz *z)
{
    memset(z, 0, sizeof(*z));
    z->window = (uint8_t *)malloc(SQX_MAX_DIC_SIZE + 0x10000);
    z->imgbuf = (uint8_t *)malloc(SQX_MAX_DIC_SIZE);
    if (!z->window || !z->imgbuf)
        return 0;
    /* Maximum sizes: 311 litlen codes (2.0) and 56 distance codes (1.1);
     * num_code is adjusted per block depending on format version. */
    if (!sqx_huff_alloc(&z->bitct, SQX_BCODES, SQX_BCODE_BITS) ||
        !sqx_huff_alloc(&z->litlen[0], SQX_NC2, SQX_LCODE_BITS) ||
        !sqx_huff_alloc(&z->litlen[1], SQX_NC2, SQX_LCODE_BITS) ||
        !sqx_huff_alloc(&z->rep, SQX_LEN_CODES, SQX_LCODE_BITS) ||
        !sqx_huff_alloc(&z->dist, SQX_DIST_CODES, SQX_LCODE_BITS) ||
        !sqx_huff_alloc(&z->low16, SQX_DIST_LOW16, SQX_LCODE_BITS))
        return 0;
    for (int i = 0; i < 4; i++)
        if (!sqx_rc_model_create(&z->mm[i], 256, 8, 1u << 12, 1))
            return 0;
    return 1;
}

void sqx_lz_free(sqx_lz *z)
{
    free(z->window);
    free(z->imgbuf);
    free(z->out);
    sqx_huff_free(&z->bitct);
    sqx_huff_free(&z->litlen[0]);
    sqx_huff_free(&z->litlen[1]);
    sqx_huff_free(&z->rep);
    sqx_huff_free(&z->dist);
    sqx_huff_free(&z->low16);
    for (int i = 0; i < 4; i++)
        sqx_rc_model_free(&z->mm[i]);
}

/* --------------------------------------------------------------- Output */

static void out_append(sqx_lz *z, const uint8_t *p, size_t n)
{
    if (z->out_len + n > z->out_cap) {
        size_t cap = z->out_cap ? z->out_cap : 65536;
        while (cap < z->out_len + n)
            cap *= 2;
        z->out = (uint8_t *)realloc(z->out, cap);
        z->out_cap = cap;
    }
    memcpy(z->out + z->out_len, p, n);
    z->file_crc = sqx_crc32_raw(z->file_crc, p, n);
    z->out_len += n;
}

/* Inverse IA32 relative-call displacement transform (t_coder.c ReImage) */
static void re_image(sqx_lz *z, uint8_t *buf, uint32_t len)
{
    int32_t last_count, image_size;

    if (len < 0x100) {
        z->image_last_count += (int32_t)len;
        return;
    }
    last_count = z->image_last_count;
    image_size = z->image_size;

    for (int32_t i = 0; i < (int32_t)len - 7; i++) {
        if (buf[i] == 0xE8) {                     /* IA32_IMGAGE_TAG */
            int32_t *p = (int32_t *)(buf + i + 1);
            int32_t a = *p;
            if (a == (-2147483647 - 1) || a == 2147483647) {
                i += 4;
                continue;
            }
            if (a > 0 && a < image_size)
                *p = a - (i + last_count);
            else if ((-a) < (i + last_count) && (-a) > 0)
                *p = image_size + a;
            i += 4;
        }
    }
    z->image_last_count += (int32_t)len;
}

static void lz_init_conv_lz(sqx_lz *z)
{
    if (z->dlt_type == SQX_DCODER_8BIT)
        for (int i = 0; i < 4; i++)
            sqx_delta_init(&z->lz_delta[i], 0, SQX_STD_DELTA_FREQ);
    else
        sqx_delta16_init(&z->lz_delta16, 0, SQX_STD_DELTA_FREQ);
    z->lz_delta_inited = 1;
}

static void lz_deconv_lz(sqx_lz *z, uint8_t *buf, uint32_t size)
{
    if (z->dlt_type == SQX_DCODER_16BIT)
        sqx_deconv_r4_16(&z->lz_delta16, buf, size);
    else
        sqx_deconv_rx(z->lz_delta, z->dlt_num, buf, size);
}

/* WriteOutBuf: post-processing transforms then chunk emission */
static void write_out_buf(sqx_lz *z, const uint8_t *p, uint32_t count)
{
    memcpy(z->imgbuf, p, count);
    if (z->dlt_code_deltas && count > 0) {
        if (z->dlt_need_ofs) {
            if (z->dlt_off_so_far + (int32_t)count > z->dlt_offset) {
                int32_t temp, index;
                z->dlt_need_ofs = 0;
                lz_init_conv_lz(z);
                temp = z->dlt_off_so_far + (int32_t)count - z->dlt_offset;
                index = (int32_t)count - temp;
                lz_deconv_lz(z, &z->imgbuf[index], count - (uint32_t)index);
            } else {
                z->dlt_off_so_far += (int32_t)count;
            }
        } else {
            lz_deconv_lz(z, z->imgbuf, count);
        }
    }
    if (z->img_flag)
        re_image(z, z->imgbuf, count);
    out_append(z, z->imgbuf, count);
}

/* WriteDictionary (arcio.c) */
static void write_dictionary(sqx_lz *z)
{
    if (z->to_flush < 1)
        return;

    if (z->curr_pos == z->dic_size && z->write_pos == z->dic_size) {
        write_out_buf(z, z->window, z->curr_pos);
        z->write_pos = 0;
        z->to_flush -= z->dic_size;
    } else if (z->curr_pos < z->write_pos) {
        uint32_t n = (uint32_t)(-(int32_t)z->write_pos) & z->mask;
        write_out_buf(z, &z->window[z->write_pos], n);
        z->to_flush -= n;
        write_out_buf(z, z->window, z->curr_pos);
        z->to_flush -= z->curr_pos;
        z->write_pos = z->curr_pos;
    } else {
        write_out_buf(z, &z->window[z->write_pos], z->curr_pos - z->write_pos);
        z->to_flush -= (z->curr_pos - z->write_pos);
        z->write_pos = z->curr_pos;
    }
}

/* ------------------------------------------- Multimedia Range Coder Blocs */

static inline uint64_t bits_pos(const sqx_bits *b);

/* Multimedia range coder parameters.
 * 1.1 (mm_coder.c): 256 symbols, increment 8, max frequency 1<<12, adaptive,
 * initial frequencies = 1.
 * 2.0 starts with initial frequencies equal to increment (8). */
static uint32_t mm_param(const char *name, uint32_t def)
{
    const char *v = getenv(name);
    return v ? (uint32_t)strtoul(v, NULL, 0) : def;
}

/* DecodeRcData (mm_coder.c): decode uSize bytes into pBuf */
static int decode_rc_data(sqx_lz *z, sqx_bits *b, uint32_t num_coder,
                          uint8_t *pBuf, uint32_t size)
{
    uint32_t count_max = (size / num_coder) * num_coder;
    uint32_t left = size - count_max;
    uint32_t count = 0, idx = 0;
    uint32_t nsym  = mm_param("SQX_MM_NSYM", 256);
    uint32_t incr  = mm_param("SQX_MM_INCR", 8);
    uint32_t maxf  = mm_param("SQX_MM_MAXFREQ", 1u << 12);
    uint32_t adapt = mm_param("SQX_MM_ADAPT", 1);
    uint32_t initf = mm_param("SQX_MM_INITFREQ", z->fmt20 ? incr : 1);
    const char *dump = getenv("SQX_MM_DUMP");
    const char *info = getenv("SQX_MM_INFO");

    num_coder = mm_param("SQX_MM_NCODER", num_coder);
    if (info) {
        FILE *f = fopen(info, "ab");
        if (f) {
            fprintf(f, "offset=%llu size=%u num_coder=%u nsym=%u incr=%u "
                       "maxfreq=%u adapt=%u initfreq=%u\n",
                    (unsigned long long)bits_pos(b), size, num_coder, nsym,
                    incr, maxf, adapt, initf);
            fclose(f);
        }
    }

    for (int i = 0; i < (int)num_coder; i++)
        if (!sqx_rc_model_init_ex(&z->mm[i], nsym, incr, maxf, adapt, initf))
            return 0;
    sqx_rc_start(&z->rc, b);

    while (count < count_max) {
        int sym = sqx_rc_decode_symbol(&z->rc, &z->mm[idx], b);
        if (sym < 0)
            goto out;
        pBuf[count++] = (uint8_t)sym;
        if (++idx == num_coder)
            idx = 0;
    }
    for (uint32_t i = 0; i < left; i++)
        pBuf[count++] = (uint8_t)sqx_bits_get(b, 8);
    if (dump) {
        FILE *f = fopen(dump, "ab");
        if (f) {
            fwrite(pBuf, 1, count, f);
            fclose(f);
        }
    }
    return 1;

out:
    if (dump) {
        FILE *f = fopen(dump, "ab");
        if (f) {
            fwrite(pBuf, 1, count, f);
            fclose(f);
        }
    }
    if (trace_on())
        printf("[trace] decode_rc_data: %u/%u symbols before failure\n", count,
               count_max);
    return 0;
}

/* ------------------------------------------------------------- Bit Header */

/* Bit position of read pointer in stream */
static inline uint64_t bits_pos(const sqx_bits *b)
{
    return (uint64_t)b->wpos * 32 + b->bits;
}

static void trace_bad_tab(const char *name, const sqx_bits *b, const sqx_huff *h)
{
    if (!trace_on())
        return;
    printf("[trace] table %s invalid at %llu bits, %u codes: ",
           name, (unsigned long long)bits_pos(b), h->num_code);
    for (uint32_t i = 0; i < h->num_code && i < 24; i++)
        printf("%u ", h->lens[i]);
    printf("...\n");
}

static int read_tabs(sqx_lz *z, sqx_bits *b, uint32_t *max_tree_code,
                     uint32_t *four_trees, int *mm_block)
{
    uint32_t block_type;
    uint32_t i;

    if (trace_on())
        printf("[trace] block header at %llu bits\n",
               (unsigned long long)bits_pos(b));
    block_type = sqx_bits_get(b, 1);

    if (block_type == 0) {                       /* LZ_BLOCK_TYPE */
        uint32_t bt, code_type = 3;              /* 3 = n/a (4 trees) */

        *mm_block = 0;
        *max_tree_code = 0;
        bt = sqx_bits_get(b, 15);
        /* 2.0: 1 bit "low16 present"; if set, 4 bits width then 16 code lengths */
        z->have16 = 0;
        z->blk_flag = 0;
        if (z->fmt20) {
            z->have16 = (int)sqx_bits_get(b, 1);
            z->blk_flag = (uint32_t)z->have16;
            if (z->have16) {
                z->low16_width = (uint8_t)sqx_bits_get(b, 4);
                for (i = 0; i < SQX_DIST_LOW16; i++)
                    z->low16.lens[i] = (uint8_t)sqx_bits_get(b, z->low16_width);
                if (!sqx_huff_generate(&z->low16)) {
                    trace_bad_tab("low16", b, &z->low16);
                    return 0;
                }
            }
        }

        *four_trees = sqx_bits_get(b, 1);
        if (trace_on())
            printf("[trace] header: bt=%u have16=%d four=%u\n",
                   bt, z->have16, *four_trees);

        /* Format-specific table sizes */
        z->litlen[0].num_code = z->litlen[1].num_code =
            z->fmt20 ? SQX_NC2 : SQX_NC;
        z->dist.num_code = z->fmt20 ? SQX_DIST_CODES2 : SQX_DIST_CODES;

        if (*four_trees != 1) {
            code_type = sqx_bits_get(b, 1);
            if (code_type == 0) {               /* CODE_TREE_TABS */
                for (i = 0; i < z->bitct.num_code; i++)
                    z->bitct.lens[i] = (uint8_t)sqx_bits_get(b, 4);
                if (!sqx_huff_generate(&z->bitct)) {
                    trace_bad_tab("bitct", b, &z->bitct);
                    return 0;
                }
                if (!sqx_read_one_tab_tree(&z->litlen[0], b, &z->bitct))
                    { trace_bad_tab("litlen", b, &z->litlen[0]); return 0; }
                if (!sqx_read_one_tab_tree(&z->rep, b, &z->bitct))
                    { trace_bad_tab("rep", b, &z->rep); return 0; }
                if (!sqx_read_one_tab_tree(&z->dist, b, &z->bitct))
                    { trace_bad_tab("dist", b, &z->dist); return 0; }
            } else {                            /* CODE_FLAT_TABS */
                if (!sqx_read_one_tab_flat(&z->litlen[0], b))
                    { trace_bad_tab("litlen", b, &z->litlen[0]); return 0; }
                if (!sqx_read_one_tab_flat(&z->rep, b))
                    { trace_bad_tab("rep", b, &z->rep); return 0; }
                if (!sqx_read_one_tab_flat(&z->dist, b))
                    { trace_bad_tab("dist", b, &z->dist); return 0; }
            }
        } else {
            *max_tree_code = sqx_bits_get(b, 9);
            if (trace_on())
                printf("[trace] 4 trees, boundary=%u\n", *max_tree_code);
            for (i = 0; i < z->bitct.num_code; i++)
                z->bitct.lens[i] = (uint8_t)sqx_bits_get(b, 4);
            if (!sqx_huff_generate(&z->bitct)) {
                trace_bad_tab("bitct", b, &z->bitct);
                return 0;
            }
            if (!sqx_read_one_tab_tree(&z->litlen[0], b, &z->bitct))
                { trace_bad_tab("litlen0", b, &z->litlen[0]); return 0; }
            if (!sqx_read_one_tab_tree(&z->litlen[1], b, &z->bitct))
                { trace_bad_tab("litlen1", b, &z->litlen[1]); return 0; }
            if (!sqx_read_one_tab_flat(&z->rep, b))
                { trace_bad_tab("rep", b, &z->rep); return 0; }
            if (!sqx_read_one_tab_flat(&z->dist, b))
                { trace_bad_tab("dist", b, &z->dist); return 0; }
        }
        if (trace_on()) {
            printf("[trace] LZ block bt=%u four=%u code_type=%u\n",
                   bt, *four_trees, code_type);
            trace_lens("bitct", &z->bitct, 19);
            trace_lens("litlen0", &z->litlen[0], 32);
            trace_lens("rep", &z->rep, 25);
            trace_lens("dist", &z->dist, 56);
        }
        if (z->ir) {
            sqx_ir_block *blk = sqx_ir_new_block(z->ir);
            if (!blk)
                return -1;
            blk->kind = 0;
            blk->bt = bt;
            blk->blk_flag = (uint8_t)z->blk_flag;
            blk->have16 = (uint8_t)z->have16;
            blk->low16_width = z->low16_width;
            memcpy(blk->low16_lens, z->low16.lens, SQX_DIST_LOW16);
            blk->four_trees = (uint8_t)*four_trees;
            blk->code_type = (uint8_t)code_type;
            blk->max_tree_code = *max_tree_code;
            memcpy(blk->bitct_lens, z->bitct.lens, SQX_BCODES);
            blk->num_litlen = z->litlen[0].num_code;
            blk->num_rep = z->rep.num_code;
            blk->num_dist = z->dist.num_code;
            memcpy(blk->litlen_lens[0], z->litlen[0].lens, blk->num_litlen);
            memcpy(blk->litlen_lens[1], z->litlen[1].lens, blk->num_litlen);
            memcpy(blk->rep_lens, z->rep.lens, SQX_LEN_CODES);
            memcpy(blk->dist_lens, z->dist.lens, SQX_DIST_CODES);
        }
        return (int)bt;                          /* Number of signals in block */
    } else {                                     /* MM_BLOCK_TYPE */
        uint32_t delta_base, delta_start, num_delta, num_entropy, bt;
        uint64_t mm_begin;

        *mm_block = 1;
        delta_base = sqx_bits_get(b, 1);
        delta_start = sqx_bits_get(b, 3);
        num_delta = sqx_bits_get(b, 2) + 1;
        num_entropy = sqx_bits_get(b, 2) + 1;
        bt = sqx_bits_get(b, 15);

        if (trace_on())
            printf("[trace] MM block: base=%u start=%u ndelta=%u nentropy=%u "
                   "bt=%u (cur=%u)\n", delta_base, delta_start,
                   num_delta, num_entropy, bt, z->curr_pos);

        mm_begin = bits_pos(b);
        if (getenv("SQX_MM_SKIP"))
            sqx_bits_dump(b, (unsigned)atoi(getenv("SQX_MM_SKIP")));
        if (!decode_rc_data(z, b, num_entropy, &z->window[z->curr_pos], bt)) {
            if (trace_on())
                printf("[trace] decode_rc_data failed at %llu bits\n",
                       (unsigned long long)bits_pos(b));
            return -1;
        }
        if (z->ir) {
            sqx_ir_block *blk = sqx_ir_new_block(z->ir);
            if (!blk)
                return -1;
            blk->kind = 1;
            blk->bt = bt;
            blk->mm_dbase = (uint8_t)delta_base;
            blk->mm_dstart = (uint8_t)delta_start;
            blk->mm_ndelta = num_delta;
            blk->mm_nentropy = num_entropy;
            blk->mm_begin = mm_begin;
            blk->mm_end = bits_pos(b);
        }
        /* Standard DeconvRType delta */
        {
            uint32_t dfreq = SQX_STD_DELTA_FREQ;
            const char *e = getenv("SQX_MM_DELTA_FREQ");
            if (e)
                dfreq = (uint32_t)strtoul(e, NULL, 0);
            sqx_deconv_rtype(delta_start, delta_base, num_delta, dfreq,
                             &z->window[z->curr_pos], bt);
        }
        return (int)bt;
    }
}

/* Read distance code (DistCTab) followed by its extra bits.
 * In 2.0, if low16 table is present and code requires >= 4 extra bits,
 * lower 4 bits are Huffman-coded via low16 and remaining high bits are read raw.
 * Returns final distance (dist + 1). */
static uint32_t read_dist_code(sqx_lz *z, sqx_bits *b, sqx_op *op)
{
    uint32_t code = sqx_huff_get(&z->dist, b);
    uint32_t dist, ebits;

    op->b = (uint16_t)code;
    dist = z->dtabs.base_table[code];
    ebits = z->dtabs.base_bits[code];
    if (ebits > 0) {
        if (z->fmt20 && z->have16 && ebits >= 4) {
            uint32_t hi = (ebits > 4) ? sqx_bits_get(b, ebits - 4) : 0;
            uint32_t low = sqx_huff_get(&z->low16, b);
            op->use16 = 1;
            op->dhi = (uint16_t)hi;
            op->dlow = (uint8_t)low;
            dist += (hi << 4) + low;
        } else {
            op->c = sqx_bits_get(b, ebits);
            dist += op->c;
        }
    }
    return dist + 1;
}

/* ------------------------------------------------------------- LZ Loop */

int sqx_lz_decode(sqx_lz *z, const uint8_t *src, size_t src_len,
                  uint64_t orig_size, int solid, uint32_t dic_size,
                  uint8_t comp_flags, uint8_t arcer_ver)
{
    sqx_bits b;
    uint64_t orig_left = orig_size;
    int32_t bt_block = 0;
    uint32_t max_tree_code = 0, four_trees = 0, last_code = 0;

    sqx_bits_init(&b, src, src_len);
    z->fmt20 = arcer_ver >= SQX_ARCVER_20;
    z->dic_size = dic_size;
    z->mask = dic_size - 1;
    z->to_flush = orig_size;
    z->out_len = 0;
    z->file_crc = SQX_CRC32_INIT;
    sqx_set_base_dist_tabs(&z->dtabs, dic_size);

    if (!solid) {
        z->last_len = z->last_dist = z->prev_idx = z->write_pos = z->curr_pos = 0;
        memset(z->prev_dists, 0, sizeof(z->prev_dists));
    }

    /* Transform states reset per file (memset DeltaCtrl) */
    z->ext_compressor = 0;
    z->ext_present = 0;
    z->dlt_code_deltas = 0;
    z->dlt_need_ofs = 0;
    z->dlt_off_so_far = 0;
    z->lz_delta_inited = 0;
    z->img_flag = 0;
    z->image_size = (int32_t)orig_size;
    z->image_last_count = 0;

    if (comp_flags != 0) {
        uint8_t hdr[2];
        hdr[0] = (uint8_t)sqx_bits_get(&b, 8);
        hdr[1] = (uint8_t)sqx_bits_get(&b, 8);
        z->ext_compressor = (uint16_t)(hdr[0] | (hdr[1] << 8));
        z->ext_present = 1;

        if (z->ext_compressor & SQX_DELTA_TYPE_FLAG) {
            uint8_t dh[6];
            for (int i = 0; i < 6; i++)
                dh[i] = (uint8_t)sqx_bits_get(&b, 8);
            z->dlt_type = dh[0];
            z->dlt_num = dh[1];
            memcpy(&z->dlt_offset, &dh[2], 4);
            z->dlt_off_so_far = 0;
            z->dlt_need_ofs = 1;
            z->dlt_code_deltas = 1;
        }
        if (z->ext_compressor & SQX_IA32IMAGE_TYPE_FLAG)
            z->img_flag = 1;
    }

    while (orig_left > 0) {
        uint32_t code_num, raw_code, sym, which = 0;
        sqx_op op;

        if (bt_block == 0) {
            int mm_block = 0;
            int r = read_tabs(z, &b, &max_tree_code, &four_trees, &mm_block);
            if (r < 0)
                return 0;
            if (mm_block) {
                uint32_t n = (uint32_t)r;
                z->curr_pos += n;
                orig_left -= n;
                bt_block = 0;
                if (z->curr_pos >= z->dic_size)
                    write_dictionary(z);
                z->curr_pos &= z->mask;
                continue;
            }
            bt_block = r;
            if (bt_block <= 0)
                return 0;
            last_code = max_tree_code;
        }

        if (four_trees == 0) {
            code_num = sqx_huff_get(&z->litlen[0], &b);
        } else {
            if (last_code < max_tree_code + 1) {
                which = 0;
                code_num = last_code = sqx_huff_get(&z->litlen[0], &b);
            } else {
                which = 1;
                code_num = last_code = sqx_huff_get(&z->litlen[1], &b);
            }
        }
        bt_block--;
        raw_code = code_num;
        memset(&op, 0, sizeof(op));
        op.code = (uint16_t)raw_code;
        op.which = (uint8_t)which;

        /* 2.0: two extra codes (257 and 285) shift following groups */
        sym = raw_code;
        if (!z->fmt20)
            sym = raw_code + (raw_code > 256 ? 1u : 0u)
                         + (raw_code > 283 ? 1u : 0u);

        if (trace_on())
            printf("[trace] op: sym=%u (bt=%d, remaining %llu bytes)\n",
                   sym, bt_block, (unsigned long long)orig_left);

        if (sym < 256) {                         /* Literal */
            if (z->ir)
                sqx_ir_add_op(z->ir, &op);
            z->window[z->curr_pos] = (uint8_t)sym;
            if (++z->curr_pos >= z->dic_size)
                write_dictionary(z);
            z->curr_pos &= z->mask;
            orig_left--;
            continue;
        } else if (sym == 256) {                 /* Repeat last match */
            uint32_t len = z->last_len, dist = z->last_dist;
            if (z->ir)
                sqx_ir_add_op(z->ir, &op);
            z->dist_state = dist;                /* 2.0: symbol 257 */
            z->prev_dists[z->prev_idx++ & 3] = dist;
            while (len--) {
                z->window[z->curr_pos] = z->window[(z->curr_pos - dist) & z->mask];
                if (++z->curr_pos >= z->dic_size)
                    write_dictionary(z);
                z->curr_pos &= z->mask;
            }
            orig_left -= z->last_len;
        } else if (sym == SQX_SYM_COPY1) {       /* 2.0: Copy 1 byte */
            uint32_t dist = z->dist_state;
            if (z->ir)
                sqx_ir_add_op(z->ir, &op);
            if (dist == 0 ||
                (uint64_t)dist > (uint64_t)z->out_len +
                                 (uint32_t)(z->curr_pos - z->write_pos))
                return 0;                        /* Safety guard: nothing to copy */
            z->window[z->curr_pos] = z->window[(z->curr_pos - dist) & z->mask];
            if (++z->curr_pos >= z->dic_size)
                write_dictionary(z);
            z->curr_pos &= z->mask;
            orig_left--;
            continue;
        } else if (sym < SQX_LEN2_START2) {      /* Repeated offset (k = 1..4) */
            uint32_t len, ebits, k;
            k = sym - 257;
            z->last_dist = z->prev_dists[(z->prev_idx - k) & 3];
            if (trace_on() && z->last_dist > SQX_MIN_LEN3)
                printf("[trace] repeated offset k=%u dist=%u%s\n", k,
                       z->last_dist,
                       z->last_dist > 0x7FFFu ? " (>0x7fff)" : "");

            code_num = sqx_huff_get(&z->rep, &b);
            op.b = (uint16_t)code_num;
            if (code_num == SQX_LEN_CODES - 1) {
                op.l14 = (uint16_t)sqx_bits_get(&b, 14);
                z->last_len = 257 + op.l14;
            } else {
                z->last_len = sqx_len_offsets[code_num];
                ebits = sqx_len_ebits[code_num];
                if (ebits > 0) {
                    op.c = sqx_bits_get(&b, ebits);
                    z->last_len += op.c;
                }
                z->last_len += 2;
                if (z->fmt20) {
                    if (z->last_dist > 0x7FFFu)
                        z->last_len++;
                } else {
                    if (z->last_dist > SQX_MIN_LEN3)
                        z->last_len++;
                    if (z->last_dist > SQX_MIN_LEN4)
                        z->last_len++;
                }
            }
            if (z->ir)
                sqx_ir_add_op(z->ir, &op);
            z->dist_state = z->last_dist;
            len = z->last_len;
            orig_left -= len;
            z->prev_dists[z->prev_idx++ & 3] = z->last_dist;
            while (len--) {
                z->window[z->curr_pos] = z->window[(z->curr_pos - z->last_dist) & z->mask];
                if (++z->curr_pos >= z->dic_size)
                    write_dictionary(z);
                z->curr_pos &= z->mask;
            }
        } else if (sym < SQX_LEN3_START2) {      /* Length-2 match */
            uint32_t ebits, dist;
            code_num = sym - SQX_LEN2_START2;
            dist = sqx_len2_offsets[code_num];
            ebits = sqx_len2_ebits[code_num];
            if (ebits > 0) {
                op.a = (uint16_t)sqx_bits_get(&b, ebits);
                dist += op.a;
            }
            dist++;
            if (z->ir)
                sqx_ir_add_op(z->ir, &op);
            orig_left -= 2;
            z->dist_state = dist;
            z->prev_dists[z->prev_idx++ & 3] = dist;
            for (int i = 0; i < 2; i++) {
                z->window[z->curr_pos] = z->window[(z->curr_pos - dist) & z->mask];
                if (++z->curr_pos >= z->dic_size)
                    write_dictionary(z);
                z->curr_pos &= z->mask;
            }
        } else if (sym < SQX_LEN_START2) {       /* Length-3 match */
            uint32_t ebits, dist;
            const uint32_t *ofs, *ebs;
            code_num = sym - SQX_LEN3_START2;
            ofs = z->fmt20 ? sqx_len3_offsets2 : sqx_len3_offsets;
            ebs = z->fmt20 ? sqx_len3_ebits2 : sqx_len3_ebits;
            dist = ofs[code_num];
            ebits = ebs[code_num];
            if (ebits > 0) {
                op.a = (uint16_t)sqx_bits_get(&b, ebits);
                dist += op.a;
            }
            dist++;
            if (z->ir)
                sqx_ir_add_op(z->ir, &op);
            orig_left -= 3;
            z->dist_state = dist;
            z->prev_dists[z->prev_idx++ & 3] = dist;
            for (int i = 0; i < 3; i++) {
                z->window[z->curr_pos] = z->window[(z->curr_pos - dist) & z->mask];
                if (++z->curr_pos >= z->dic_size)
                    write_dictionary(z);
                z->curr_pos &= z->mask;
            }
        } else {                                 /* Match > 3 */
            uint32_t len, dist, ebits;

            if (sym == SQX_NC2 - 1) {            /* Long length (14 bits) */
                op.l14 = (uint16_t)sqx_bits_get(&b, 14);
                len = 257 + op.l14;
                dist = read_dist_code(z, &b, &op);
            } else {
                code_num = sym - SQX_LEN_START2;
                len = sqx_len_offsets[code_num];
                ebits = sqx_len_ebits[code_num];
                if (ebits > 0) {
                    op.a = (uint16_t)sqx_bits_get(&b, ebits);
                    len += op.a;
                }
                len += 4;
                dist = read_dist_code(z, &b, &op);
                if (trace_on() && (dist > SQX_MIN_LEN3 || op.use16))
                    printf("[trace] match dist=%u len=%u code=%u "
                           "ebits=%u base=%u (code2.0=%u)\n",
                           dist, len, op.b, z->dtabs.base_bits[op.b],
                           z->dtabs.base_table[op.b], op.code);
                if (!z->fmt20 && dist > SQX_MIN_LEN4)
                    len++;
            }
            if (z->ir)
                sqx_ir_add_op(z->ir, &op);
            orig_left -= len;
            z->dist_state = dist;
            z->prev_dists[z->prev_idx++ & 3] = dist;
            while (len--) {
                z->window[z->curr_pos] = z->window[(z->curr_pos - dist) & z->mask];
                if (++z->curr_pos >= z->dic_size)
                    write_dictionary(z);
                z->curr_pos &= z->mask;
            }
        }
    }

    write_dictionary(z);
    return 1;
}

/* ------------------------------------------------------------------- IR */

int sqx_lz_decode_ir(sqx_lz *z, const uint8_t *src, size_t src_len,
                     uint64_t orig_size, int solid, uint32_t dic_size,
                     uint8_t comp_flags, uint8_t arcer_ver, sqx_ir *ir)
{
    int ok;

    sqx_ir_init(ir, arcer_ver, comp_flags, dic_size, orig_size);
    z->ir = ir;
    ok = sqx_lz_decode(z, src, src_len, orig_size, solid, dic_size,
                       comp_flags, arcer_ver);
    z->ir = NULL;
    ir->ext_compressor = z->ext_compressor;
    if (z->dlt_code_deltas) {
        ir->dlt_type = (uint8_t)z->dlt_type;
        ir->dlt_num = (uint8_t)z->dlt_num;
        ir->dlt_offset = z->dlt_offset;
    }
    return ok;
}
