/* ir.c — Intermediate Representation (IR) of an SQX LZ/Huffman stream (see ir.h) */
#include "ir.h"
#include <stdlib.h>
#include <string.h>

void sqx_ir_init(sqx_ir *ir, int arcer_ver, uint8_t comp_flags,
                 uint32_t dic_size, uint64_t orig_size)
{
    memset(ir, 0, sizeof(*ir));
    ir->arcer_ver = arcer_ver;
    ir->fmt20 = arcer_ver >= 20;
    ir->comp_flags = comp_flags;
    ir->dic_size = dic_size;
    ir->orig_size = orig_size;
}

void sqx_ir_free(sqx_ir *ir)
{
    free(ir->ops);
    free(ir->blocks);
    memset(ir, 0, sizeof(*ir));
}

sqx_ir_block *sqx_ir_new_block(sqx_ir *ir)
{
    if (ir->num_blocks == ir->cap_blocks) {
        size_t cap = ir->cap_blocks ? ir->cap_blocks * 2 : 16;
        sqx_ir_block *p = (sqx_ir_block *)realloc(ir->blocks, sizeof(*p) * cap);
        if (!p)
            return NULL;
        ir->blocks = p;
        ir->cap_blocks = cap;
    }
    {
        sqx_ir_block *b = &ir->blocks[ir->num_blocks++];
        memset(b, 0, sizeof(*b));
        b->op_first = ir->num_ops;
        return b;
    }
}

void sqx_ir_add_op(sqx_ir *ir, const sqx_op *op)
{
    if (ir->num_ops == ir->cap_ops) {
        size_t cap = ir->cap_ops ? ir->cap_ops * 2 : 4096;
        sqx_op *p = (sqx_op *)realloc(ir->ops, sizeof(*p) * cap);
        if (!p)
            return;
        ir->ops = p;
        ir->cap_ops = cap;
    }
    ir->ops[ir->num_ops++] = *op;
    if (ir->num_blocks)
        ir->blocks[ir->num_blocks - 1].op_count++;
}
