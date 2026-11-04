/* ultra.h — SQX 2.0 Method 7 ("Ultra") decompression
 * LZ77 + Carryless Range Coder + Fenwick Tree Probability Models
 * Pure C99, freestanding, zero external dependencies.
 */
#ifndef SQX_ULTRA_H
#define SQX_ULTRA_H

#include <stddef.h>
#include <stdint.h>
#include "bitio.h"

#define SQX_ULTRA_RC_TOP  (1u << 24)
#define SQX_ULTRA_RC_BOT  (1u << 16)

/* Fenwick-tree based adaptive probability model */
typedef struct {
    uint32_t *freq;       /* Symbol frequencies: freq[0..nsym-1] */
    uint32_t *tree;       /* Fenwick tree: tree[0]=freq[0], tree[1..nsym-1] */
    uint32_t nsym;
    uint32_t tot_freq;
    uint32_t incr;
    uint32_t max_freq;
    uint32_t power_of_2;  /* Largest power of 2 <= nsym */
} sqx_ultra_fenwick;

/* Binary bit model for range coder */
typedef struct {
    uint32_t freq[2];
    uint32_t tot_freq;
    uint32_t incr;
    uint32_t max_freq;
} sqx_ultra_bitmodel;

/* Nibble-decomposed byte model: 1 high nibble (16 sym) + 16 low nibbles (16 sym) */
typedef struct {
    sqx_ultra_fenwick high_model;
    sqx_ultra_fenwick low_models[16];
} sqx_ultra_bytemodel;

typedef struct {
    uint32_t low;
    uint32_t range;
    uint32_t code;
} sqx_ultra_rc;

typedef struct {
    /* Range coder & bit reader */
    sqx_ultra_rc rc;
    sqx_bits bits;

    /* Window circular buffer */
    uint8_t *win;
    uint32_t win_size;
    uint32_t win_mask;
    uint32_t win_pos;

    /* Decoded output buffer */
    uint8_t *out;
    size_t out_len;
    size_t out_cap;

    /* LZ state machine */
    uint32_t state;
    uint32_t prev_state;
    uint32_t prev_byte;
    uint32_t rep[4];
    uint32_t saved_rep_dist;
    uint32_t saved_rep_len;

    /* Single contiguous allocation for all Fenwick model freq/tree arrays */
    uint32_t *model_pool;

    /* Probability models */
    sqx_ultra_bitmodel is_match[64];
    sqx_ultra_bitmodel is_rep[16];

    sqx_ultra_bytemodel lit_state0[256];
    sqx_ultra_bytemodel lit_state_other[256];

    sqx_ultra_fenwick dist_slot[4];
    sqx_ultra_fenwick dist_low4;

    sqx_ultra_fenwick len_slot[4];
    sqx_ultra_fenwick rep_len_slot[4];

    sqx_ultra_fenwick len_sub[4][9];
    sqx_ultra_fenwick rep_len_sub[4][9];

    sqx_ultra_fenwick rep_index[4];

    int initialized;
} sqx_ultra;

void sqx_ultra_init(sqx_ultra *u);
void sqx_ultra_free(sqx_ultra *u);
int  sqx_ultra_decode(sqx_ultra *u, const uint8_t *data, size_t dlen,
                      uint64_t orig_size, int solid, uint32_t dict_size);

#endif /* SQX_ULTRA_H */

