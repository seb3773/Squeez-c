/* ir.h — Intermediate Representation (IR) of an SQX LZ/Huffman stream.
 *
 * An LZ block is described by its header parameters + its code length tables
 * + its list of operations (with extra bits values). This contains all
 * required information to re-emit the stream: the encoder reconstructs
 * canonical codewords and table serializations.
 *
 * Multimedia blocks (kind = 1) store their bit span from the source archive.
 */
#ifndef SQX_IR_H
#define SQX_IR_H

#include "huff.h"
#include <stdint.h>

typedef struct {
    uint16_t code;    /* Raw symbol from litlen table */
    uint16_t a;       /* Extra bits for 1st symbol (len2/len3/len dist/rep len) */
    uint16_t b;       /* 2nd symbol: rep table (repeated offset) or distance */
    uint32_t c;       /* Extra bits for 2nd symbol (up to 19 bits for 8 MB dict) */
    uint32_t dhi;     /* 2.0 low16: high distance bits (excluding lower 4 bits) */
    uint8_t  dlow;    /* 2.0 low16: symbol in lower 4 bits table */
    uint8_t  use16;   /* 2.0: distance encoded via low16 table */
    uint16_t l14;     /* Long length (14 bits) if code == NC-2 */
    uint8_t  which;   /* Selected litlen tree (0 or 1) */
    uint16_t adv;     /* Diagnostics: bytes emitted */
} sqx_op;

typedef struct {
    int      kind;                     /* 0 = LZ, 1 = multimedia */
    uint32_t bt;                       /* Signal count in block */
    uint8_t  blk_flag;                 /* Additional v2.0 header flag */
    uint8_t  have16;                   /* v2.0: low 4-bit distance table present */
    uint8_t  low16_width;              /* v2.0: bit width for low16 lengths */
    uint8_t  low16_lens[SQX_DIST_LOW16];
    uint8_t  four_trees;
    uint8_t  code_type;
    uint32_t max_tree_code;
    uint8_t  bitct_lens[SQX_BCODES];
    uint32_t num_litlen, num_rep, num_dist;
    uint8_t  litlen_lens[2][SQX_NC2];
    uint8_t  rep_lens[SQX_LEN_CODES];
    uint8_t  dist_lens[SQX_DIST_CODES];
    uint64_t mm_begin, mm_end;         /* kind 1: bit range in source stream */
    uint8_t  mm_dbase, mm_dstart;      /* kind 1: MM block header */
    uint32_t mm_ndelta, mm_nentropy;   /* kind 1: delta / entropy counts */
    size_t   op_first, op_count;
} sqx_ir_block;

typedef struct {
    int      arcer_ver;
    int      fmt20;
    uint8_t  comp_flags;
    uint16_t ext_compressor;
    uint8_t  dlt_type, dlt_num;
    int32_t  dlt_offset;
    uint32_t dic_size;
    uint64_t orig_size;

    size_t       num_ops, cap_ops;
    sqx_op      *ops;
    size_t       num_blocks, cap_blocks;
    sqx_ir_block *blocks;
} sqx_ir;

void sqx_ir_init(sqx_ir *ir, int arcer_ver, uint8_t comp_flags,
                 uint32_t dic_size, uint64_t orig_size);
void sqx_ir_free(sqx_ir *ir);
sqx_ir_block *sqx_ir_new_block(sqx_ir *ir);
void sqx_ir_add_op(sqx_ir *ir, const sqx_op *op);

#endif /* SQX_IR_H */
