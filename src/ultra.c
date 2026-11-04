/* ultra.c — SQX 2.0 Method 7 ("Ultra") decompression engine
 * LZ77 + Carryless Range Coder + Fenwick Tree Probability Models
 * Pure C99, freestanding, zero external dependencies.
 */
#include "ultra.h"
#include <stdlib.h>
#include <string.h>

#define TOTAL_MODEL_DWORDS 283216u

static const uint32_t ULTRA_DIST_BASE[50] = {
    0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768,
    1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152,
    65536, 98304, 131072, 196608, 262144, 393216, 524288, 786432, 1048576, 1572864,
    2097152, 3145728, 4194304, 6291456, 8388608, 12582912, 16777216, 25165824
};

static const uint8_t ULTRA_DIST_EXTRA[50] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16,
    17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 23
};

static const uint32_t ULTRA_LEN_BASE[9] = { 2, 3, 4, 6, 10, 18, 34, 66, 130 };
static const uint8_t ULTRA_LEN_NSYM[9]  = { 0, 0, 2, 4, 8, 16, 32, 64, 128 };
static const uint8_t ULTRA_LEN_INCR[9]  = { 0, 0, 1, 1, 2,  2,  2,  4,   4 };

/* --- Range Coder Core --- */

static void rc_start(sqx_ultra_rc *rc, sqx_bits *b)
{
    rc->low = 0;
    rc->range = 0xFFFFFFFFu;
    rc->code = 0;
    for (int i = 0; i < 4; i++) {
        rc->code = (rc->code << 8) | sqx_bits_get(b, 8);
    }
}

static inline void rc_normalize(sqx_ultra_rc *rc, sqx_bits *b)
{
    while ((rc->low ^ (rc->low + rc->range)) < SQX_ULTRA_RC_TOP ||
           (rc->range < SQX_ULTRA_RC_BOT &&
            ((rc->range = (0u - rc->low) & (SQX_ULTRA_RC_BOT - 1)), 1))) {
        rc->code = (rc->code << 8) | sqx_bits_get(b, 8);
        rc->range <<= 8;
        rc->low <<= 8;
    }
}

static uint32_t rc_decode_bits(sqx_ultra_rc *rc, sqx_bits *b, int nbits)
{
    uint32_t val = 0;
    for (int i = 0; i < nbits; i++) {
        rc->range >>= 1;
        uint32_t bit;
        if (rc->code - rc->low < rc->range) {
            bit = 0;
        } else {
            bit = 1;
            rc->low += rc->range;
        }
        val = (val << 1) | bit;
        rc_normalize(rc, b);
    }
    return val;
}

/* --- Bit Model --- */

static void bit_model_init(sqx_ultra_bitmodel *m, uint32_t incr, uint32_t max_freq)
{
    m->incr = incr;
    m->max_freq = max_freq;
    m->freq[0] = incr;
    m->freq[1] = incr;
    m->tot_freq = incr * 2;
}

static void bit_model_reset(sqx_ultra_bitmodel *m)
{
    m->freq[0] = m->incr;
    m->freq[1] = m->incr;
    m->tot_freq = m->incr * 2;
}

static inline int bit_model_decode(sqx_ultra_bitmodel *m, sqx_ultra_rc *rc, sqx_bits *b)
{
    uint32_t r_step = rc->range / m->tot_freq;
    rc->range = r_step;
    uint32_t count;
    if (r_step == 0)
        count = m->tot_freq;
    else
        count = (rc->code - rc->low) / r_step;

    int bit;
    if (count < m->freq[0]) {
        bit = 0;
        rc->range = r_step * m->freq[0];
        m->freq[0] += m->incr;
    } else if (count < m->tot_freq) {
        bit = 1;
        rc->low += r_step * m->freq[0];
        rc->range = r_step * m->freq[1];
        m->freq[1] += m->incr;
    } else {
        bit = 0;
    }
    rc_normalize(rc, b);

    m->tot_freq += m->incr;
    if (m->tot_freq > m->max_freq) {
        m->freq[0] -= (m->freq[0] >> 1);
        m->freq[1] -= (m->freq[1] >> 1);
        m->tot_freq = m->freq[0] + m->freq[1];
    }
    return bit;
}

/* --- Fenwick Model --- */

static void fenwick_reset(sqx_ultra_fenwick *m)
{
    m->tot_freq = m->nsym * m->incr;
    for (uint32_t i = 0; i < m->nsym; i++) {
        m->freq[i] = m->incr;
        m->tree[i] = 0;
    }
    m->tree[0] = m->incr;
    for (uint32_t i = 1; i < m->nsym; i++) {
        uint32_t idx = i;
        while (idx < m->nsym) {
            m->tree[idx] += m->incr;
            idx += idx & (-idx);
        }
    }
}

static void fenwick_init(sqx_ultra_fenwick *m, uint32_t nsym, uint32_t incr, uint32_t max_freq,
                         uint32_t *freq_buf, uint32_t *tree_buf)
{
    m->nsym = nsym;
    m->incr = incr;
    m->max_freq = max_freq;
    m->freq = freq_buf;
    m->tree = tree_buf;

    uint32_t p = 1;
    while (nsym > p) {
        p <<= 1;
    }
    p >>= 1;
    m->power_of_2 = p;

    fenwick_reset(m);
}

static uint32_t fenwick_decode(sqx_ultra_fenwick *m, sqx_ultra_rc *rc, sqx_bits *b)
{
    uint32_t r_step = rc->range / m->tot_freq;
    rc->range = r_step;
    uint32_t count;
    if (r_step == 0)
        count = m->tot_freq;
    else
        count = (rc->code - rc->low) / r_step;

    if (count >= m->tot_freq)
        return 0;

    uint32_t sym;
    uint32_t cum_freq;
    if (count < m->tree[0]) {
        sym = 0;
        cum_freq = 0;
    } else {
        uint32_t cnt = count - m->tree[0] + 1;
        uint32_t idx = 0;
        uint32_t step = m->power_of_2;
        while (step > 0) {
            uint32_t cand = idx + step;
            if (cand < m->nsym && cnt > m->tree[cand]) {
                idx = cand;
                cnt -= m->tree[cand];
            }
            step >>= 1;
        }
        sym = idx + 1;
        if (sym >= m->nsym)
            sym = m->nsym - 1;

        cum_freq = m->tree[0];
        uint32_t k = sym - 1;
        while (k > 0) {
            cum_freq += m->tree[k];
            k &= (k - 1);
        }
    }

    rc->low += r_step * cum_freq;
    rc->range = r_step * m->freq[sym];
    rc_normalize(rc, b);

    /* Update model */
    uint32_t incr = m->incr;
    m->freq[sym] += incr;
    if (sym == 0) {
        m->tree[0] += incr;
    } else {
        uint32_t idx = sym;
        while (idx < m->nsym) {
            m->tree[idx] += incr;
            idx += idx & (-idx);
        }
    }
    m->tot_freq += incr;

    if (m->tot_freq > m->max_freq) {
        for (uint32_t i = 0; i < m->nsym; i++) {
            uint32_t d = m->freq[i] >> 1;
            if (i == 0) {
                m->tree[0] -= d;
            } else {
                uint32_t idx = i;
                while (idx < m->nsym) {
                    m->tree[idx] -= d;
                    idx += idx & (-idx);
                }
            }
            m->tot_freq -= d;
            m->freq[i] -= d;
        }
    }

    return sym;
}

/* --- Byte Model --- */

static inline uint32_t byte_model_decode(sqx_ultra_bytemodel *m, sqx_ultra_rc *rc, sqx_bits *b)
{
    uint32_t hi = fenwick_decode(&m->high_model, rc, b);
    uint32_t lo = fenwick_decode(&m->low_models[hi], rc, b);
    return (hi << 4) | lo;
}

/* --- Ultra Engine Initialization --- */

void sqx_ultra_init(sqx_ultra *u)
{
    memset(u, 0, sizeof(*u));
    u->model_pool = (uint32_t *)malloc(sizeof(uint32_t) * TOTAL_MODEL_DWORDS);
    if (!u->model_pool)
        return;

    uint32_t *p = u->model_pool;

    /* Binary bit models */
    for (int i = 0; i < 64; i++)
        bit_model_init(&u->is_match[i], 8, 1023);
    for (int i = 0; i < 16; i++)
        bit_model_init(&u->is_rep[i], 8, 1023);

    /* Literal models for state 0 */
    for (int i = 0; i < 256; i++) {
        fenwick_init(&u->lit_state0[i].high_model, 16, 1, 511, p, p + 16);
        p += 32;
        for (int j = 0; j < 16; j++) {
            fenwick_init(&u->lit_state0[i].low_models[j], 16, 1, 511, p, p + 16);
            p += 32;
        }
    }

    /* Literal models for state other */
    for (int i = 0; i < 256; i++) {
        fenwick_init(&u->lit_state_other[i].high_model, 16, 2, 511, p, p + 16);
        p += 32;
        for (int j = 0; j < 16; j++) {
            fenwick_init(&u->lit_state_other[i].low_models[j], 16, 2, 511, p, p + 16);
            p += 32;
        }
    }

    /* Distance models */
    for (int i = 0; i < 4; i++) {
        fenwick_init(&u->dist_slot[i], 50, 4, 1023, p, p + 50);
        p += 100;
    }
    fenwick_init(&u->dist_low4, 16, 4, 1023, p, p + 16);
    p += 32;

    /* Length slot models */
    for (int i = 0; i < 4; i++) {
        fenwick_init(&u->len_slot[i], 9, 8, 1023, p, p + 9);
        p += 18;
    }
    for (int i = 0; i < 4; i++) {
        fenwick_init(&u->rep_len_slot[i], 9, 8, 1023, p, p + 9);
        p += 18;
    }

    /* Length sub-models */
    for (int st = 0; st < 4; st++) {
        for (int sl = 0; sl < 9; sl++) {
            uint32_t nsym = ULTRA_LEN_NSYM[sl];
            if (nsym > 0) {
                fenwick_init(&u->len_sub[st][sl], nsym, ULTRA_LEN_INCR[sl], 255, p, p + nsym);
                p += nsym * 2;
            }
        }
    }
    for (int st = 0; st < 4; st++) {
        for (int sl = 0; sl < 9; sl++) {
            uint32_t nsym = ULTRA_LEN_NSYM[sl];
            if (nsym > 0) {
                fenwick_init(&u->rep_len_sub[st][sl], nsym, ULTRA_LEN_INCR[sl], 255, p, p + nsym);
                p += nsym * 2;
            }
        }
    }

    /* Rep index models */
    for (int i = 0; i < 4; i++) {
        fenwick_init(&u->rep_index[i], 6, 4, 1023, p, p + 6);
        p += 12;
    }
}

void sqx_ultra_free(sqx_ultra *u)
{
    if (u->model_pool) {
        free(u->model_pool);
        u->model_pool = NULL;
    }
    if (u->win) {
        free(u->win);
        u->win = NULL;
    }
    if (u->out) {
        free(u->out);
        u->out = NULL;
    }
    u->initialized = 0;
}

static void ultra_reset_models(sqx_ultra *u)
{
    for (int i = 0; i < 64; i++)
        bit_model_reset(&u->is_match[i]);
    for (int i = 0; i < 16; i++)
        bit_model_reset(&u->is_rep[i]);

    for (int i = 0; i < 256; i++) {
        fenwick_reset(&u->lit_state0[i].high_model);
        for (int j = 0; j < 16; j++)
            fenwick_reset(&u->lit_state0[i].low_models[j]);
    }

    for (int i = 0; i < 256; i++) {
        fenwick_reset(&u->lit_state_other[i].high_model);
        for (int j = 0; j < 16; j++)
            fenwick_reset(&u->lit_state_other[i].low_models[j]);
    }

    for (int i = 0; i < 4; i++)
        fenwick_reset(&u->dist_slot[i]);
    fenwick_reset(&u->dist_low4);

    for (int i = 0; i < 4; i++) {
        fenwick_reset(&u->len_slot[i]);
        fenwick_reset(&u->rep_len_slot[i]);
    }

    for (int st = 0; st < 4; st++) {
        for (int sl = 2; sl < 9; sl++) {
            fenwick_reset(&u->len_sub[st][sl]);
            fenwick_reset(&u->rep_len_sub[st][sl]);
        }
    }

    for (int i = 0; i < 4; i++)
        fenwick_reset(&u->rep_index[i]);
}

static uint32_t ultra_decode_length(sqx_ultra *u, int is_rep)
{
    sqx_ultra_fenwick *slot_model = is_rep ? &u->rep_len_slot[u->state] : &u->len_slot[u->state];
    uint32_t slot = fenwick_decode(slot_model, &u->rc, &u->bits);
    if (slot >= 9)
        slot = 8;

    uint32_t sub = 0;
    if (ULTRA_LEN_NSYM[slot] > 0) {
        sqx_ultra_fenwick *sub_model = is_rep ? &u->rep_len_sub[u->state][slot] : &u->len_sub[u->state][slot];
        sub = fenwick_decode(sub_model, &u->rc, &u->bits);
    }
    uint32_t length = ULTRA_LEN_BASE[slot] + sub;
    if (length == 257) {
        length = 257 + rc_decode_bits(&u->rc, &u->bits, 14);
    }
    return length;
}

/* --- Decompression Entry Point --- */

int sqx_ultra_decode(sqx_ultra *u, const uint8_t *data, size_t dlen,
                     uint64_t orig_size, int solid, uint32_t dict_size)
{
    if (orig_size > 0xFFFFFFFFu)
        return 0;

    if (!u->model_pool) {
        sqx_ultra_init(u);
        if (!u->model_pool)
            return 0;
    }

    /* Window buffer management */
    if (dict_size == 0)
        dict_size = 4194304; /* Default 4 MB */
    /* Ensure power of 2 */
    uint32_t p2 = 32768;
    while (p2 < dict_size && p2 < 0x40000000u)
        p2 <<= 1;
    dict_size = p2;

    if (!u->win || u->win_size != dict_size) {
        if (u->win) free(u->win);
        u->win = (uint8_t *)malloc(dict_size);
        if (!u->win) return 0;
        u->win_size = dict_size;
        u->win_mask = dict_size - 1;
        u->win_pos = 0;
        memset(u->win, 0, dict_size);
        u->initialized = 0;
    }

    /* Output buffer */
    if (orig_size > u->out_cap) {
        size_t ncap = (size_t)orig_size + 1024;
        uint8_t *nout = (uint8_t *)realloc(u->out, ncap);
        if (!nout) return 0;
        u->out = nout;
        u->out_cap = ncap;
    }
    u->out_len = 0;

    if (orig_size == 0)
        return 1;

    /* Handle solid continuation */
    if (!solid || !u->initialized) {
        ultra_reset_models(u);
        u->win_pos = 0;
        memset(u->win, 0, u->win_size);
    }

    /* Reset per-stream state */
    u->state = 0;
    u->prev_state = 0;
    u->prev_byte = 0;
    u->rep[0] = u->rep[1] = u->rep[2] = u->rep[3] = 0;
    u->saved_rep_dist = 0;
    u->saved_rep_len = 0;

    sqx_bits_init(&u->bits, data, dlen);
    rc_start(&u->rc, &u->bits);

    uint64_t pos = 0;

    while (u->out_len < orig_size) {
        uint32_t pos_state = (uint32_t)(pos & 3);
        uint32_t ctx_match = (u->prev_state * 4 + u->state) * 4 + pos_state;
        int is_m = bit_model_decode(&u->is_match[ctx_match], &u->rc, &u->bits);

        if (is_m == 0) {
            /* Literal */
            sqx_ultra_bytemodel *bm = (u->state == 0) ? &u->lit_state0[u->prev_byte] : &u->lit_state_other[u->prev_byte];
            uint32_t sym = byte_model_decode(bm, &u->rc, &u->bits);

            u->win[u->win_pos] = (uint8_t)sym;
            u->win_pos = (u->win_pos + 1) & u->win_mask;
            u->out[u->out_len++] = (uint8_t)sym;

            u->prev_byte = sym;
            u->prev_state = u->state;
            u->state = 0;
            pos++;
        } else {
            /* Match or Repeat match */
            uint32_t ctx_rep = u->prev_state * 4 + u->state;
            int is_rep = bit_model_decode(&u->is_rep[ctx_rep], &u->rc, &u->bits);

            uint32_t dist, length, next_state;

            if (is_rep) {
                uint32_t rep_idx = fenwick_decode(&u->rep_index[u->state], &u->rc, &u->bits);
                if (rep_idx == 4) {
                    dist = u->rep[0] + 1;
                    length = 1;
                    next_state = 3;
                } else if (rep_idx == 5) {
                    dist = u->saved_rep_dist + 1;
                    length = u->saved_rep_len;
                    next_state = 2;
                } else {
                    if (rep_idx > 3) rep_idx = 3;
                    length = ultra_decode_length(u, 1);
                    uint32_t rep_dist = u->rep[rep_idx];
                    u->saved_rep_len = length;
                    u->saved_rep_dist = rep_dist;
                    for (uint32_t i = rep_idx; i > 0; i--) {
                        u->rep[i] = u->rep[i - 1];
                    }
                    u->rep[0] = rep_dist;
                    dist = rep_dist + 1;
                    next_state = 2;
                }
            } else {
                length = ultra_decode_length(u, 0);
                uint32_t dist_idx = (length == 2) ? 0 : (length == 3) ? 1 : (length == 4) ? 2 : 3;
                uint32_t dist_slot = fenwick_decode(&u->dist_slot[dist_idx], &u->rc, &u->bits);
                if (dist_slot >= 50) dist_slot = 49;

                uint32_t base = ULTRA_DIST_BASE[dist_slot];
                uint32_t extra = ULTRA_DIST_EXTRA[dist_slot];
                if (extra == 0) {
                    dist = base;
                } else if (extra < 4) {
                    dist = base + rc_decode_bits(&u->rc, &u->bits, extra);
                } else {
                    uint32_t direct = rc_decode_bits(&u->rc, &u->bits, extra - 4);
                    uint32_t low4 = fenwick_decode(&u->dist_low4, &u->rc, &u->bits);
                    dist = base + (direct << 4) + low4;
                }

                u->rep[3] = u->rep[2];
                u->rep[2] = u->rep[1];
                u->rep[1] = u->rep[0];
                u->rep[0] = dist;
                dist = dist + 1;
                next_state = 1;
            }

            /* Copy match bytes */
            for (uint32_t i = 0; i < length && u->out_len < orig_size; i++) {
                uint32_t src_pos = (u->win_pos - dist) & u->win_mask;
                uint8_t b = u->win[src_pos];
                u->win[u->win_pos] = b;
                u->win_pos = (u->win_pos + 1) & u->win_mask;
                u->out[u->out_len++] = b;
            }

            u->prev_byte = u->win[(u->win_pos - dist) & u->win_mask];
            u->prev_state = u->state;
            u->state = next_state;
            pos += length;
        }
    }

    u->initialized = 1;
    return (u->out_len == orig_size);
}

