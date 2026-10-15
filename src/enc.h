/* enc.h — SQX stream encoder (LZ + Huffman) from Intermediate Representation.
 *
 * `src`/`src_len` are the source bitstream: only used to pass through
 * multimedia range-coded blocks when reproducing an existing stream.
 * Can be NULL if the IR contains only LZ blocks.
 *
 * Returns 1 on success, 0 on error.
 */
#ifndef SQX_ENC_H
#define SQX_ENC_H

#include "bitw.h"
#include "ir.h"

int sqx_enc_ir(const sqx_ir *ir, const uint8_t *src, size_t src_len,
               sqx_bitw *w);

/* Diagnostics (populated by sqx_enc_ir) */
extern unsigned long sqx_enc_tree_mismatch;
extern unsigned long sqx_enc_ops_written;

#endif /* SQX_ENC_H */
