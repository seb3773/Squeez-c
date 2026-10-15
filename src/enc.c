/* enc.c — SQX stream encoder (LZ + Huffman) from IR (see enc.h).
 *
 * The produced stream is the exact mirror of what sqx_lz_decode consumes:
 * bits written in identical order, 32-bit little-endian words zero-padded.
 */
#include "enc.h"
#include "lz.h"
#include "henc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned long sqx_enc_tree_mismatch = 0;
unsigned long sqx_enc_ops_written = 0;

static int enc_trace;
static int enc_trace_on(void)
{
    if (enc_trace == 0)
        enc_trace = getenv("SQX_ENC_TRACE") ? 2 : -1;
    return enc_trace == 2;
}

/* One byte at a time, LSB to MSB (matching decoder assembly of little-endian int32) */
static void put_le_bytes(sqx_bitw *w, uint32_t val, unsigned nbytes)
{
    for (unsigned i = 0; i < nbytes; i++)
        SQX_PUT(w, (val >> (8 * i)) & 0xFF, 8);
}

#define PUT_SYM(bw, codes, lens, sym) \
    SQX_PUT((bw), (codes)[(sym)], (lens)[(sym)])

/* ------------------------------------------- Bit copy (multimedia blocks) */

static int copy_bits(sqx_bitw *w, sqx_bits *rs, uint64_t from, uint64_t to)
{
    uint64_t have = (uint64_t)rs->wpos * 32 + rs->bits;

    if (have > from)
        return 0;                          /* Overlap: inconsistent IR */
    if (have < from)
        sqx_bits_dump(rs, (unsigned)(from - have));
    while (to > from) {
        unsigned n = (unsigned)(to - from > 32 ? 32 : to - from);
        SQX_PUT(w, sqx_bits_get(rs, n), n);
        from += n;
    }
    return 1;
}

/* ----------------------------------------------------------------- Tables */

/* Serialize block header and tables matching decoder read_tabs */
static int write_lz_block_header(const sqx_ir *ir, const sqx_ir_block *b,
                                 sqx_bitw *w, sqx_dist_tab *dt)
{
    uint16_t bcodes[SQX_BCODES], lcodes[2][SQX_NC2];
    uint16_t rcodes[SQX_LEN_CODES], dcodes[SQX_DIST_CODES];
    uint16_t hcodes[SQX_DIST_LOW16];
    uint8_t  plens[SQX_LEN_CODES];
    int treed = 0;

    SQX_PUT(w, 0, 1);                      /* LZ_BLOCK_TYPE */
    SQX_PUT(w, b->bt, 15);
    if (ir->fmt20) {
        SQX_PUT(w, b->have16, 1);
        if (b->have16) {
            SQX_PUT(w, b->low16_width, 4);
            for (uint32_t i = 0; i < SQX_DIST_LOW16; i++)
                SQX_PUT(w, b->low16_lens[i], b->low16_width);
        }
    }
    SQX_PUT(w, b->four_trees, 1);

    if (b->four_trees != 1) {
        SQX_PUT(w, b->code_type, 1);
        treed = (b->code_type == 0);
    } else {
        SQX_PUT(w, b->max_tree_code, 9);
        treed = 1;
    }

    if (treed) {
        if (!sqx_make_codes(b->bitct_lens, SQX_BCODES, SQX_BCODE_BITS, bcodes))
            return 0;
        for (uint32_t i = 0; i < SQX_BCODES; i++)
            SQX_PUT(w, b->bitct_lens[i], 4);
    }

    if (!sqx_make_codes(b->litlen_lens[0], b->num_litlen, SQX_LCODE_BITS,
                        lcodes[0]))
        return 0;
    if (b->four_trees == 1 &&
        !sqx_make_codes(b->litlen_lens[1], b->num_litlen, SQX_LCODE_BITS,
                        lcodes[1]))
        return 0;
    if (!sqx_make_codes(b->rep_lens, b->num_rep, SQX_LCODE_BITS, rcodes))
        return 0;
    if (!sqx_make_codes(b->dist_lens, b->num_dist, SQX_LCODE_BITS, dcodes))
        return 0;

    if (b->four_trees != 1 && b->code_type != 0) {          /* Flat tables */
        sqx_write_flat_tab(w, b->litlen_lens[0], b->num_litlen);
        sqx_write_flat_tab(w, b->rep_lens, b->num_rep);
        sqx_write_flat_tab(w, b->dist_lens, b->num_dist);
    } else {
        sqx_write_tree_tab(w, b->litlen_lens[0], b->num_litlen, bcodes,
                           b->bitct_lens);
        if (b->four_trees == 1) {
            sqx_write_tree_tab(w, b->litlen_lens[1], b->num_litlen, bcodes,
                               b->bitct_lens);
            sqx_write_flat_tab(w, b->rep_lens, b->num_rep);
            sqx_write_flat_tab(w, b->dist_lens, b->num_dist);
        } else {                                 /* 3 trees, code_type = 0 */
            sqx_write_tree_tab(w, b->rep_lens, b->num_rep, bcodes,
                               b->bitct_lens);
            sqx_write_tree_tab(w, b->dist_lens, b->num_dist, bcodes,
                               b->bitct_lens);
        }
    }

    if (ir->fmt20 && b->have16 &&
        !sqx_make_codes(b->low16_lens, SQX_DIST_LOW16, SQX_LCODE_BITS,
                        hcodes))
        return 0;

    sqx_set_base_dist_tabs(dt, ir->dic_size);
    if (enc_trace_on())
        fprintf(stderr, "[enc] dict=%u -> %u dist codes (base_bits[36]=%u)\n",
                ir->dic_size, dt->num_codes, dt->base_bits[36]);
    (void)plens;
    (void)dcodes;
    (void)hcodes;
    return 1;
}

/* ------------------------------------------------------------- Operations */

static int write_ops(const sqx_ir *ir, const sqx_ir_block *b, sqx_bitw *w,
                     const sqx_dist_tab *dt)
{
    uint16_t lcodes[2][SQX_NC2];
    uint16_t rcodes[SQX_LEN_CODES], dcodes[SQX_DIST_CODES];
    uint16_t hcodes[SQX_DIST_LOW16];
    const uint32_t *len3_ebits = ir->fmt20 ? sqx_len3_ebits2 : sqx_len3_ebits;
    uint32_t last_code = b->max_tree_code;

    if (!sqx_make_codes(b->litlen_lens[0], b->num_litlen, SQX_LCODE_BITS, lcodes[0]))
        return 0;
    if (b->four_trees == 1 &&
        !sqx_make_codes(b->litlen_lens[1], b->num_litlen, SQX_LCODE_BITS, lcodes[1]))
        return 0;
    if (!sqx_make_codes(b->rep_lens, b->num_rep, SQX_LCODE_BITS, rcodes))
        return 0;
    if (!sqx_make_codes(b->dist_lens, b->num_dist, SQX_LCODE_BITS, dcodes))
        return 0;
    if (ir->fmt20 && b->have16 &&
        !sqx_make_codes(b->low16_lens, SQX_DIST_LOW16, SQX_LCODE_BITS,
                        hcodes))
        return 0;

    for (size_t i = 0; i < b->op_count; i++) {
        const sqx_op *op = &ir->ops[b->op_first + i];
        uint32_t code = op->code, t = 0, sym;

        if (b->four_trees == 1) {
            if (last_code < b->max_tree_code + 1) {
                t = 0;
                last_code = code;
            } else {
                t = 1;
                last_code = code;
            }
            if (op->which != t)
                sqx_enc_tree_mismatch++;
        }

        PUT_SYM(w, lcodes[t], b->litlen_lens[t], code);

        /* Same projection as decoder: unified 2.0 symbol alphabet */
        sym = code;
        if (!ir->fmt20)
            sym = code + (code > 256 ? 1u : 0u) + (code > 283 ? 1u : 0u);

        if (sym < SQX_LEN2_START2) {
            if (sym > 257) {
                uint32_t rsym = op->b;
                if (rsym >= b->num_rep)
                    return 0;
                PUT_SYM(w, rcodes, b->rep_lens, rsym);
                if (rsym == SQX_LEN_CODES - 1)
                    SQX_PUT(w, op->l14, 14);
                else if (sqx_len_ebits[rsym] > 0)
                    SQX_PUT(w, op->c, sqx_len_ebits[rsym]);
            }
        } else if (sym < SQX_LEN3_START2) {            /* Length 2 */
            uint32_t k = sym - SQX_LEN2_START2;
            if (sqx_len2_ebits[k] > 0)
                SQX_PUT(w, op->a, sqx_len2_ebits[k]);
        } else if (sym < SQX_LEN_START2) {             /* Length 3 */
            uint32_t k = sym - SQX_LEN3_START2;
            if (len3_ebits[k] > 0)
                SQX_PUT(w, op->a, len3_ebits[k]);
        } else {                                       /* Length >= 4 */
            if (sym == SQX_NC2 - 1) {
                SQX_PUT(w, op->l14, 14);
            } else {
                uint32_t k = sym - SQX_LEN_START2;
                if (k >= SQX_LEN_CODES)
                    return 0;
                if (sqx_len_ebits[k] > 0)
                    SQX_PUT(w, op->a, sqx_len_ebits[k]);
            }
            if (op->b >= b->num_dist)
                return 0;
            PUT_SYM(w, dcodes, b->dist_lens, op->b);
            if (dt->base_bits[op->b] > 0) {
                uint32_t ebits = dt->base_bits[op->b];
                if (ir->fmt20 && b->have16 && ebits >= 4) {
                    if (!op->use16) {
                        fprintf(stderr, "enc: distance %u missing low16 bits\n",
                                op->b);
                        return 0;
                    }
                    if (ebits > 4)
                        SQX_PUT(w, op->dhi, ebits - 4);
                    PUT_SYM(w, hcodes, b->low16_lens, op->dlow);
                } else {
                    if (getenv("SQX_ENC_OPS") && ebits >= 15)
                        fprintf(stderr, "[encops] sym=%u dist_code=%u ebits=%u "
                                "c=%u -> dist=%u\n", sym, op->b, ebits, op->c,
                                dt->base_table[op->b] + op->c + 1);
                    SQX_PUT(w, op->c, ebits);
                }
            }
        }
        sqx_enc_ops_written++;
    }
    return 1;
}

/* ----------------------------------------------------------------- Stream */

int sqx_enc_ir(const sqx_ir *ir, const uint8_t *src, size_t src_len,
               sqx_bitw *w)
{
    sqx_bits rs;
    sqx_dist_tab dt;

    sqx_enc_tree_mismatch = 0;
    sqx_enc_ops_written = 0;
    static const uint8_t empty[8] = {0};

    memset(&dt, 0, sizeof(dt));
    sqx_bits_init(&rs, src ? src : empty, src ? src_len : 0);

    if (ir->comp_flags != 0) {
        SQX_PUT(w, ir->ext_compressor, 16);
        if (ir->ext_compressor & SQX_DELTA_TYPE_FLAG) {
            put_le_bytes(w, ir->dlt_type, 1);
            put_le_bytes(w, ir->dlt_num, 1);
            put_le_bytes(w, (uint32_t)ir->dlt_offset, 4);
        }
    }

    for (size_t i = 0; i < ir->num_blocks; i++) {
        const sqx_ir_block *b = &ir->blocks[i];

        if (b->kind == 0) {
            uint64_t b0 = sqx_bitw_bits(w);
            if (!write_lz_block_header(ir, b, w, &dt))
                return 0;
            if (enc_trace_on())
                fprintf(stderr, "[enc] block %zu header: %llu bits\n", i,
                        (unsigned long long)(sqx_bitw_bits(w) - b0));
            if (!write_ops(ir, b, w, &dt))
                return 0;
        } else {
            SQX_PUT(w, 1, 1);                          /* MM_BLOCK_TYPE */
            SQX_PUT(w, b->mm_dbase, 1);
            SQX_PUT(w, b->mm_dstart, 3);
            SQX_PUT(w, b->mm_ndelta - 1, 2);
            SQX_PUT(w, b->mm_nentropy - 1, 2);
            SQX_PUT(w, b->bt, 15);
            if (!src) {
                fprintf(stderr, "enc: multimedia block without source stream\n");
                return 0;
            }
            if (!copy_bits(w, &rs, b->mm_begin, b->mm_end))
                return 0;
        }
        if (enc_trace_on())
            fprintf(stderr, "[enc] block %zu kind=%d bt=%u ops=%zu\n",
                    i, b->kind, b->bt, b->op_count);
    }
    sqx_bitw_flush(w);
    if (enc_trace_on())
        fprintf(stderr, "[enc] done: len=%zu bits=%llu ops=%lu\n", w->len,
                (unsigned long long)sqx_bitw_bits(w), sqx_enc_ops_written);
    return !w->err;
}
