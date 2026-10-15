/* henc.h — SQX Huffman encoder tables and serialization.
 *
 * - Canonical code assignment (identical to GenerateUCTab from ctab.c);
 * - Code length array serialization (inverse of ReadOneTabFlat / ReadOneTabTree);
 * - Length-limited Huffman tree generation from symbol frequencies (for `create`).
 */
#ifndef SQX_HENC_H
#define SQX_HENC_H

#include "bitio.h"
#include "bitw.h"
#include <stdint.h>

/* Canonical code assignment. `codes[sym]` receives the codeword written
 * on `lens[sym]` bits (MSB first). Returns 1 if the tree is complete (Kraft == 1). */
int sqx_make_codes(const uint8_t *lens, uint32_t n, uint32_t mb, uint16_t *codes);

/* Write code lengths using flat format (CODE_FLAT_TABS) */
void sqx_write_flat_tab(sqx_bitw *w, const uint8_t *lens, uint32_t n);

/* Write code lengths using tree format (CODE_TREE_TABS) with meta-alphabet `bitct`
 * (matching ReadOneTabTree conventions). */
void sqx_write_tree_tab(sqx_bitw *w, const uint8_t *lens, uint32_t n,
                        const uint16_t *bitct_codes, const uint8_t *bitct_lens);

typedef struct {
    unsigned pos_bits;      /* First bit of table in source stream */
    unsigned nbits;         /* Number of bits consumed by decoder */
} sqx_tab_span;

/* Generate length-limited Huffman code lengths (Huffman + ceiling) from frequencies.
 * `lens` (n bytes) is filled; returns 0 on error. */
int sqx_lens_from_freq(const uint32_t *freq, uint32_t n, uint32_t limit,
                       uint8_t *lens);

/* Count meta-symbols (0..18) emitted by tree serialization for a length array
 * (matching sqx_write_tree_tab rules). */
void sqx_meta_freq(const uint8_t *lens, uint32_t n, uint32_t *mfreq);

#endif /* SQX_HENC_H */
