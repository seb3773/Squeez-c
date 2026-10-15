/* ppm8.c — PPMd var. I ("Ppmd8") codec = SQX 2.0 Method 6.
 *
 * Port of C/Ppmd8.c + C/Ppmd8Dec.c (7-Zip, public domain, based on Dmitry
 * Shkarin's PPMd var. I) as found in sqx20u.dll, coupled with the SQX
 * carryless range coder (from 1.1 rcoder.c).
 */
#include "ppm8.h"
#include "bitio.h"
#include "bitw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PPMD_NUM_INDEXES 38
#define PPMD_INT_BITS    7
#define PPMD_PERIOD_BITS 7
#define PPMD_BIN_SCALE   (1u << (PPMD_INT_BITS + PPMD_PERIOD_BITS))
#define PPMD8_MIN_ORDER  2
#define PPMD8_MAX_ORDER  16
#define MAX_FREQ         124
#define UNIT_SIZE        12
#define EMPTY_NODE       0xFFFFFFFFu
#define FLAG_RESCALED    0x04u
#define FLAG_PREV_HIGH   0x10u

static const uint8_t k_exp_escape[16] =
    { 25, 14, 9, 7, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 2, 2 };
static const uint16_t k_init_bin_esc[8] =
    { 0x3CDD, 0x1F3F, 0x59BF, 0x48F3, 0x64A1, 0x5ABC, 0x6632, 0x6051 };

typedef struct {
    uint16_t summ;
    uint8_t  shift, count;
} ppm_see;

struct sqx_ppm8 {
    /* Model tables (Ppmd8_Construct) */
    uint8_t  indx2units[PPMD_NUM_INDEXES + 2];
    uint8_t  units2indx[128];
    uint8_t  ns2bsindx[256];
    uint8_t  ns2indx[260];
    uint8_t  exp_escape[16];
    ppm_see  dummy_see;
    ppm_see  see[24][32];
    uint16_t binsumm[25][64];

    /* Model arena: `base` is allocated memory block, references are offsets */
    uint8_t *base;
    uint32_t size, align_offset;
    uint32_t freelist[PPMD_NUM_INDEXES];
    uint32_t stamps[PPMD_NUM_INDEXES];
    uint32_t glue_count;
    uint32_t lo_unit, hi_unit, text, units_start;

    /* Current context */
    uint32_t min_ctx, max_ctx, found_state;
    unsigned order_fall, init_esc, prev_success, max_order;
    int32_t  run_length, init_rl;
    int      have_model;

    /* Seen symbols mask (0 = seen in higher context) */
    uint8_t  mask[256];
    unsigned num_masked;

    /* Unmasked states of low context (escape loop) */
    uint8_t *low_states[256];
    unsigned n_low;

    /* range coder SQX */
    sqx_bits bits;
    uint32_t rc_low, rc_range, rc_code;

    /* output of encoder (sqx_ppm8_encode) */
    sqx_bitw *enc;
    int      enc_err;
};

/* ---------------------------------------------------------------- bytes --- */

static uint32_t rd16(const uint8_t *q)
{
    return (uint32_t)q[0] | ((uint32_t)q[1] << 8);
}

static uint32_t rd32(const uint8_t *q)
{
    return rd16(q) | (rd16(q + 2) << 16);
}

static void wr16(uint8_t *q, uint32_t v)
{
    q[0] = (uint8_t)(v & 0xFFu);
    q[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr32(uint8_t *q, uint32_t v)
{
    wr16(q, v);
    wr16(q + 2, v >> 16);
}

/* Ppmd8_Context = 12 bytes; CPpmd_State = 6 bytes (Symbol, Freq, 32-bit ref) */
#define PTR(p, off)   ((p)->base + (off))
#define REF(p, ptr)   ((uint32_t)((ptr) - (p)->base))

#define CTX_NS(q)     ((unsigned)(q)[0])                 /* NumStats */
#define CTX_FLAGS(q)  ((unsigned)(q)[1])
#define CTX_SUMM(q)   rd16((q) + 2)                      /* SummFreq */
#define CTX_STATS(q)  rd32((q) + 4)                      /* states reference */
#define CTX_SUFFIX(q) rd32((q) + 8)
#define ONE_STATE(q)  ((q) + 2)                          /* single-state context */

#define ST_SYM(q)     ((unsigned)(q)[0])
#define ST_FREQ(q)    ((unsigned)(q)[1])
#define ST_SUCC(q)    rd32((q) + 2)

static uint32_t u2b(uint32_t nu) { return nu * UNIT_SIZE; }

static unsigned u2i(const struct sqx_ppm8 *p, uint32_t nu)
{
    return (unsigned)p->units2indx[nu - 1];
}

static uint32_t i2u(const struct sqx_ppm8 *p, unsigned indx)
{
    return (uint32_t)p->indx2units[indx];
}

static void set_succ(uint8_t *state, uint32_t v)
{
    wr32(state + 2, v);
}

/* ------------------------------------------------------------ range coder - */

#define RC_TOP (1u << 24)
#define RC_BOT (1u << 16)

static void rc_norm(struct sqx_ppm8 *p)
{
    for (;;) {
        if ((p->rc_low ^ (p->rc_low + p->rc_range)) >= RC_TOP) {
            if (p->rc_range >= RC_BOT)
                break;
            p->rc_range = (0u - p->rc_low) & (RC_BOT - 1u);
        }
        p->rc_code = (p->rc_code << 8) | sqx_bits_get(&p->bits, 8);
        p->rc_range <<= 8;
        p->rc_low <<= 8;
    }
}

/* RC_GetThreshold : range /= total ; (code - low) / range */
static uint32_t rc_threshold(struct sqx_ppm8 *p, uint32_t total)
{
    if (total == 0)
        return 0xFFFFFFFFu;
    p->rc_range /= total;
    if (p->rc_range == 0)
        return 0xFFFFFFFFu;
    return (p->rc_code - p->rc_low) / p->rc_range;
}

/* 0x10022fb0 : low += range*start ; range *= size (sans normalisation) */
static void rc_decode(struct sqx_ppm8 *p, uint32_t start, uint32_t size)
{
    p->rc_low += p->rc_range * start;
    p->rc_range *= size;
}

/* 0x10022e90: uniform decode of n bits (MSB first) */
static uint32_t rc_get_bits(struct sqx_ppm8 *p, unsigned n)
{
    uint32_t v = 0;
    while (n--) {
        uint32_t r = p->rc_range >> 1;
        uint32_t q;
        p->rc_range = r;
        if (r == 0)
            return v;
        q = (p->rc_code - p->rc_low) / r;
        v = (v << 1) | q;
        p->rc_low += q * r;
        rc_norm(p);
    }
    return v;
}

/* -------------------------------------------------- allocations internes --- */

static void insert_node(struct sqx_ppm8 *p, uint8_t *node, unsigned indx)
{
    wr32(node, EMPTY_NODE);
    wr32(node + 4, p->freelist[indx]);
    wr32(node + 8, i2u(p, indx));
    p->freelist[indx] = REF(p, node);
    p->stamps[indx]++;
}

static uint8_t *remove_node(struct sqx_ppm8 *p, unsigned indx)
{
    uint8_t *node = PTR(p, p->freelist[indx]);
    p->freelist[indx] = rd32(node + 4);
    p->stamps[indx]--;
    return node;
}

static void split_block(struct sqx_ppm8 *p, uint8_t *ptr, unsigned old_indx,
                        unsigned new_indx)
{
    uint32_t nu = i2u(p, old_indx) - i2u(p, new_indx);
    unsigned i;
    ptr += u2b(i2u(p, new_indx));
    i = u2i(p, nu);
    if (i2u(p, i) != nu) {
        uint32_t k = i2u(p, --i);
        insert_node(p, ptr + u2b(k), (unsigned)(nu - k - 1));
    }
    insert_node(p, ptr, i);
}

static void glue_free_blocks(struct sqx_ppm8 *p)
{
    uint32_t n = 0, prev = EMPTY_NODE;  /* EMPTY_NODE sert de « &n » */
    unsigned i;

    p->glue_count = 1u << 13;
    memset(p->stamps, 0, sizeof(p->stamps));
    if (p->lo_unit != p->hi_unit)
        wr32(PTR(p, p->lo_unit), 0);    /* nœud de garde */

    for (i = 0; i < PPMD_NUM_INDEXES; i++) {
        uint32_t next = p->freelist[i];
        p->freelist[i] = 0;
        while (next != 0) {
            uint8_t *node = PTR(p, next);
            uint32_t nu = rd32(node + 8);
            if (prev == EMPTY_NODE)
                n = next;
            else
                wr32(PTR(p, prev) + 4, next);
            next = rd32(node + 4);
            if (nu != 0) {
                prev = REF(p, node);
                for (;;) {
                    uint8_t *node2 = node + u2b(nu);
                    if (rd32(node2) != EMPTY_NODE)
                        break;
                    nu += rd32(node2 + 8);
                    wr32(node2 + 8, 0);
                    wr32(node + 8, nu);
                }
            }
        }
    }
    if (prev == EMPTY_NODE)
        n = 0;
    else
        wr32(PTR(p, prev) + 4, 0);

    while (n != 0) {
        uint8_t *node = PTR(p, n);
        uint32_t nu = rd32(node + 8);
        unsigned i2;
        n = rd32(node + 4);
        if (nu == 0)
            continue;
        for (; nu > 128; nu -= 128, node += 128 * UNIT_SIZE)
            insert_node(p, node, PPMD_NUM_INDEXES - 1);
        i2 = u2i(p, nu);
        if (i2u(p, i2) != nu) {
            uint32_t k = i2u(p, --i2);
            insert_node(p, node + u2b(k), (unsigned)(nu - k - 1));
        }
        insert_node(p, node, i2);
    }
}

static uint8_t *alloc_units_rare(struct sqx_ppm8 *p, unsigned indx)
{
    unsigned i;
    if (p->glue_count == 0) {
        glue_free_blocks(p);
        if (p->freelist[indx] != 0)
            return remove_node(p, indx);
    }
    i = indx;
    do {
        if (++i == PPMD_NUM_INDEXES) {
            uint32_t num_bytes = u2b(i2u(p, indx));
            uint32_t us = p->units_start;
            p->glue_count--;
            if (us - p->text > num_bytes) {
                p->units_start = us - num_bytes;
                return PTR(p, p->units_start);
            }
            return NULL;
        }
    } while (p->freelist[i] == 0);
    {
        uint8_t *block = remove_node(p, i);
        split_block(p, block, i, indx);
        return block;
    }
}

static uint8_t *alloc_units(struct sqx_ppm8 *p, unsigned indx)
{
    if (p->freelist[indx] != 0)
        return remove_node(p, indx);
    {
        uint32_t num_bytes = u2b(i2u(p, indx));
        uint32_t lo = p->lo_unit;
        if (p->hi_unit - lo >= num_bytes) {
            p->lo_unit = lo + num_bytes;
            return PTR(p, lo);
        }
    }
    return alloc_units_rare(p, indx);
}

static void mem12(uint8_t *dest, const uint8_t *src, uint32_t num)
{
    uint32_t *d = (uint32_t *)(void *)dest;
    const uint32_t *z = (const uint32_t *)(const void *)src;
    uint32_t n = num;
    do {
        d[0] = z[0];
        d[1] = z[1];
        d[2] = z[2];
        z += 3;
        d += 3;
    } while (--n);
}

static uint8_t *shrink_units(struct sqx_ppm8 *p, uint8_t *old_ptr,
                             uint32_t old_nu, uint32_t new_nu)
{
    unsigned i0 = u2i(p, old_nu);
    unsigned i1 = u2i(p, new_nu);
    if (i0 == i1)
        return old_ptr;
    if (p->freelist[i1] != 0) {
        uint8_t *ptr = remove_node(p, i1);
        mem12(ptr, old_ptr, new_nu);
        insert_node(p, old_ptr, i0);
        return ptr;
    }
    split_block(p, old_ptr, i0, i1);
    return old_ptr;
}

static void free_units(struct sqx_ppm8 *p, uint8_t *ptr, uint32_t nu)
{
    insert_node(p, ptr, u2i(p, nu));
}

static void special_free_unit(struct sqx_ppm8 *p, uint8_t *ptr)
{
    if (ptr != PTR(p, p->units_start))
        insert_node(p, ptr, 0);
    else
        p->units_start += UNIT_SIZE;
}

static void expand_text_area(struct sqx_ppm8 *p)
{
    uint32_t count[PPMD_NUM_INDEXES];
    unsigned i;

    memset(count, 0, sizeof(count));
    if (p->lo_unit != p->hi_unit)
        wr32(PTR(p, p->lo_unit), 0);
    {
        uint8_t *node = PTR(p, p->units_start);
        while (rd32(node) == EMPTY_NODE) {
            uint32_t nu = rd32(node + 8);
            wr32(node, 0);
            count[u2i(p, nu)]++;
            node += u2b(nu);
        }
        p->units_start = REF(p, node);
    }
    for (i = 0; i < PPMD_NUM_INDEXES; i++) {
        uint32_t cnt = count[i];
        uint32_t prev, n;
        if (cnt == 0)
            continue;
        prev = EMPTY_NODE;              /* EMPTY_NODE sert de « &Freelist[i] » */
        n = p->freelist[i];
        p->stamps[i] -= cnt;
        for (;;) {
            uint8_t *node = PTR(p, n);
            uint32_t nn = rd32(node + 4);
            if (rd32(node) != 0) {
                prev = n + 4;
                n = nn;
                continue;
            }
            if (prev == EMPTY_NODE)
                p->freelist[i] = nn;
            else
                wr32(PTR(p, prev), nn);
            n = nn;
            if (--cnt == 0)
                break;
        }
    }
}

/* ------------------------------------------------------------ PPMd Model -- */

static void restart_model(struct sqx_ppm8 *p)
{
    unsigned i, k, m;

    memset(p->freelist, 0, sizeof(p->freelist));
    memset(p->stamps, 0, sizeof(p->stamps));
    p->text = p->align_offset;
    p->hi_unit = p->text + p->size;
    p->lo_unit = p->units_start =
        p->hi_unit - p->size / 8 / UNIT_SIZE * 7 * UNIT_SIZE;
    p->glue_count = 0;
    p->order_fall = p->max_order;
    p->run_length = p->init_rl =
        -(int32_t)((p->max_order < 12) ? p->max_order : 12) - 1;
    p->prev_success = 0;

    {
        uint32_t mc = (p->hi_unit -= UNIT_SIZE);
        uint32_t s = p->lo_unit;
        uint8_t *q;
        p->lo_unit += u2b(256 / 2);
        p->max_ctx = p->min_ctx = mc;
        p->found_state = s;
        q = PTR(p, mc);
        q[0] = 255;                     /* NumStats */
        q[1] = 0;                       /* Flags */
        wr16(q + 2, 256 + 1);           /* SummFreq */
        wr32(q + 4, s);                 /* Stats */
        wr32(q + 8, 0);                 /* Suffix */
        q = PTR(p, s);
        for (i = 0; i < 256; i++, q += 6) {
            q[0] = (uint8_t)i;
            q[1] = 1;
            wr32(q + 2, 0);
        }
    }

    for (i = m = 0; m < 25; m++) {
        while (p->ns2indx[i] == m)
            i++;
        for (k = 0; k < 8; k++) {
            unsigned r;
            uint16_t *dest = p->binsumm[m] + k;
            uint16_t val = (uint16_t)(PPMD_BIN_SCALE - k_init_bin_esc[k] / (i + 1));
            for (r = 0; r < 64; r += 8)
                dest[r] = val;
        }
    }
    for (i = m = 0; m < 24; m++) {
        unsigned summ;
        ppm_see *s2;
        while (p->ns2indx[i + 3] == m + 3)
            i++;
        s2 = p->see[m];
        summ = ((2 * i + 5) << (PPMD_PERIOD_BITS - 4));
        for (k = 0; k < 32; k++, s2++) {
            s2->summ = (uint16_t)summ;
            s2->shift = (uint8_t)(PPMD_PERIOD_BITS - 4);
            s2->count = 7;
        }
    }
    p->dummy_see.summ = 0;
    p->dummy_see.shift = PPMD_PERIOD_BITS;
    p->dummy_see.count = 64;

    memset(p->mask, 0xFF, sizeof(p->mask));
    p->num_masked = 0;
}

static void model_init(struct sqx_ppm8 *p, unsigned max_order,
                       unsigned restore_method)
{
    p->max_order = max_order;
    (void)restore_method;               /* pas de FREEZE : toujours RESTART */
    restart_model(p);
    p->have_model = 1;
}

/* Ppmd_See_UPDATE */
static void see_update(ppm_see *see)
{
    if (see->shift < PPMD_PERIOD_BITS && --see->count == 0) {
        see->summ = (uint16_t)(see->summ << 1);
        see->count = (uint8_t)(3u << see->shift);
        see->shift++;
    }
}

static ppm_see *make_esc_freq(struct sqx_ppm8 *p, unsigned num_masked1,
                              uint32_t *esc_freq)
{
    ppm_see *see;
    uint8_t *mc = PTR(p, p->min_ctx);
    unsigned num_stats = CTX_NS(mc);

    if (num_stats != 0xFF) {
        unsigned suffix_ns = CTX_NS(PTR(p, CTX_SUFFIX(mc)));
        unsigned idx = (unsigned)p->ns2indx[num_stats + 2] - 3;
        see = p->see[idx]
              + (CTX_SUMM(mc) > 11 * (num_stats + 1) ? 1u : 0u)
              + 2u * (2 * num_stats < suffix_ns + num_masked1 ? 1u : 0u)
              + CTX_FLAGS(mc);
        {
            unsigned summ = see->summ;
            unsigned r = summ >> see->shift;
            see->summ = (uint16_t)(summ - r);
            *esc_freq = r + (r == 0 ? 1u : 0u);
        }
    } else {
        see = &p->dummy_see;
        *esc_freq = 1;
    }
    return see;
}

static void refresh(struct sqx_ppm8 *p, uint32_t ctx, unsigned old_nu,
                    unsigned scale)
{
    uint8_t *c = PTR(p, ctx);
    unsigned i = CTX_NS(c), esc_freq, sum_freq, flags;
    uint8_t *s;

    s = shrink_units(p, PTR(p, CTX_STATS(c)), old_nu, (i + 2) >> 1);
    wr32(c + 4, REF(p, s));
    if (CTX_SUMM(c) >= (1u << 15))
        scale |= 1;
    flags = ST_SYM(s) + 0xC0u;
    {
        unsigned freq = ST_FREQ(s);
        esc_freq = CTX_SUMM(c) - freq;
        freq = (freq + scale) >> scale;
        sum_freq = freq;
        s[1] = (uint8_t)freq;
    }
    do {
        unsigned freq;
        s += 6;
        freq = ST_FREQ(s);
        esc_freq -= freq;
        freq = (freq + scale) >> scale;
        sum_freq += freq;
        s[1] = (uint8_t)freq;
        flags |= ST_SYM(s) + 0xC0u;
    } while (--i);
    wr16(c + 2, (uint32_t)(uint16_t)(sum_freq + ((esc_freq + scale) >> scale)));
    c[1] = (uint8_t)((CTX_FLAGS(c) & (FLAG_PREV_HIGH + FLAG_RESCALED * scale))
                     + ((flags >> 5) & 0x08u));
}

static uint32_t get_used_memory(const struct sqx_ppm8 *p)
{
    uint32_t v = 0;
    unsigned i;
    for (i = 0; i < PPMD_NUM_INDEXES; i++)
        v += p->stamps[i] * i2u(p, i);
    return p->size - (p->hi_unit - p->lo_unit) - (p->units_start - p->text)
           - u2b(v);
}

static uint32_t cutoff(struct sqx_ppm8 *p, uint32_t ctx, unsigned order);

static void restore_model(struct sqx_ppm8 *p, uint32_t ctx_error)
{
    uint32_t c;
    uint8_t *s;

    p->text = p->align_offset;
    for (c = p->max_ctx; c != ctx_error; c = CTX_SUFFIX(PTR(p, c))) {
        uint8_t *cc = PTR(p, c);
        uint8_t ns = (uint8_t)(CTX_NS(cc) - 1);
        cc[0] = ns;
        if (ns == 0) {
            s = PTR(p, CTX_STATS(cc));
            cc[1] = (uint8_t)((CTX_FLAGS(cc) & FLAG_PREV_HIGH)
                              + (ST_SYM(s) >= 0x40 ? 0x08u : 0u));
            cc[2] = s[0];
            cc[3] = (uint8_t)((ST_FREQ(s) + 11) >> 3);
            wr32(cc + 4, rd32(s + 2));
            special_free_unit(p, s);
        } else {
            refresh(p, c, ((unsigned)ns + 3) >> 1, 0);
        }
    }
    for (; c != p->min_ctx; c = CTX_SUFFIX(PTR(p, c))) {
        uint8_t *cc = PTR(p, c);
        if (CTX_NS(cc) == 0) {
            cc[3] = (uint8_t)((cc[3] + 1) >> 1);    /* OneState->Freq */
        } else {
            uint32_t ns = CTX_NS(cc);
            wr16(cc + 2, CTX_SUMM(cc) + 4);
            if (CTX_SUMM(cc) > 128 + 4 * ns)
                refresh(p, c, (ns + 2) >> 1, 1);
        }
    }
    if (get_used_memory(p) < (p->size >> 1)) {
        restart_model(p);
    } else {
        while (CTX_SUFFIX(PTR(p, p->max_ctx)) != 0)
            p->max_ctx = CTX_SUFFIX(PTR(p, p->max_ctx));
        do {
            cutoff(p, p->max_ctx, 0);
            expand_text_area(p);
        } while (get_used_memory(p) > 3 * (p->size >> 2));
        p->glue_count = 0;
        p->order_fall = p->max_order;
    }
    p->min_ctx = p->max_ctx;
}

/* Model update log (SQX_PPM8_DEBUG_WALK) */
static int walk_dbg;

static uint32_t create_successors(struct sqx_ppm8 *p, int skip, uint8_t *s1,
                                  uint32_t c_ref)
{
    uint32_t up_branch = ST_SUCC(PTR(p, p->found_state));
    unsigned new_sym, new_freq, flags;
    unsigned num_ps = 0;
    uint8_t *ps[PPMD8_MAX_ORDER + 1];
    uint8_t *c = PTR(p, c_ref);

    if (walk_dbg)
        fprintf(stderr, "  CS(skip=%d s1=%s c=0x%x) up_branch=0x%x found=0x%x\n",
                skip, s1 ? "set" : "NULL", (unsigned)c_ref, (unsigned)up_branch,
                (unsigned)p->found_state);
    if (!skip)
        ps[num_ps++] = PTR(p, p->found_state);
    while (CTX_SUFFIX(c) != 0) {
        uint32_t successor;
        uint8_t *s;
        c = PTR(p, CTX_SUFFIX(c));
        if (s1) {
            s = s1;
            s1 = NULL;
        } else if (CTX_NS(c) != 0) {
            unsigned sym = ST_SYM(PTR(p, p->found_state));
            unsigned lim = CTX_NS(c) + 1;
            for (s = PTR(p, CTX_STATS(c)); ST_SYM(s) != sym; s += 6)
                if (--lim == 0) {
                    fprintf(stderr, "ppm8: symbole %u absent du contexte"
                            " (ns=%u flags=0x%x summ=%u, ctx=0x%x)\n",
                            sym, CTX_NS(c), CTX_FLAGS(c), CTX_SUMM(c),
                            (unsigned)REF(p, c));
                    return 0;
                }
            if (ST_FREQ(s) < MAX_FREQ - 9) {
                s[1]++;
                wr16(c + 2, CTX_SUMM(c) + 1);
            }
        } else {
            unsigned tmp = CTX_NS(PTR(p, CTX_SUFFIX(c))) == 0 ? 1u : 0u;
            s = ONE_STATE(c);
            if (tmp && ST_FREQ(s) < 24)
                s[1]++;
        }
        successor = ST_SUCC(s);
        if (walk_dbg)
            fprintf(stderr, "    walk c=0x%x ns=%u s=0x%x sym=%u succ=0x%x\n",
                    (unsigned)REF(p, c), CTX_NS(c), (unsigned)REF(p, s),
                    ST_SYM(s), (unsigned)successor);
        if (successor != up_branch) {
            if (num_ps == 0)
                return successor;
            c = PTR(p, successor);
            break;
        }
        ps[num_ps++] = s;
    }
    if (walk_dbg)
        fprintf(stderr, "    walk done c=0x%x num_ps=%u new_sym=%u\n",
                (unsigned)REF(p, c), num_ps,
                (unsigned)PTR(p, up_branch)[0]);
    new_sym = PTR(p, up_branch)[0];
    up_branch++;
    flags = (ST_SYM(PTR(p, p->found_state)) >= 0x40 ? 0x10u : 0u)
            + (new_sym >= 0x40 ? 0x08u : 0u);
    if (CTX_NS(c) == 0) {
        new_freq = ST_FREQ(ONE_STATE(c));
    } else {
        uint32_t cf, s0;
        uint8_t *s;
        for (s = PTR(p, CTX_STATS(c)); ST_SYM(s) != new_sym; s += 6)
            ;
        cf = ST_FREQ(s) - 1;
        s0 = CTX_SUMM(c) - CTX_NS(c) - cf;
        new_freq = 1 + ((2 * cf <= s0) ? (5 * cf > s0 ? 1u : 0u)
                                       : ((cf + 2 * s0 - 3) / s0));
    }
    do {
        uint8_t *c1;
        if (p->hi_unit != p->lo_unit) {
            c1 = PTR(p, p->hi_unit -= UNIT_SIZE);
        } else if (p->freelist[0] != 0) {
            c1 = remove_node(p, 0);
        } else {
            c1 = alloc_units_rare(p, 0);
            if (!c1)
                return 0;
        }
        c1[1] = (uint8_t)flags;
        c1[0] = 0;
        c1[2] = (uint8_t)new_sym;
        c1[3] = (uint8_t)new_freq;
        set_succ(ONE_STATE(c1), up_branch);
        wr32(c1 + 8, REF(p, c));       /* c1->Suffix = REF(c) : c = contexte atteint
                                        * via suffix traversal, not c_ref */
        set_succ(ps[--num_ps], REF(p, c1));
        c_ref = REF(p, c1);
        c = c1;
        if (walk_dbg)
            fprintf(stderr, "    new ctx 0x%x ns=0 sym=%u freq=%u succ=0x%x suffix=0x%x\n",
                    (unsigned)c_ref, new_sym, (unsigned)new_freq,
                    (unsigned)up_branch, (unsigned)CTX_SUFFIX(c1));
    } while (num_ps != 0);
    return c_ref;
}

static uint32_t reduce_order(struct sqx_ppm8 *p, uint8_t *s1, uint32_t c_ref)
{
    uint8_t *s = NULL;
    uint32_t c1 = c_ref;
    uint32_t up_branch = p->text;
    uint8_t *c = PTR(p, c_ref);
    unsigned f_sym = ST_SYM(PTR(p, p->found_state));

    if (walk_dbg)
        fprintf(stderr, "  RO(c=0x%x s1=%s) up_branch=0x%x found=0x%x\n",
                (unsigned)c_ref, s1 ? "set" : "NULL", (unsigned)up_branch,
                (unsigned)p->found_state);
    set_succ(PTR(p, p->found_state), up_branch);
    p->order_fall++;
    for (;;) {
        if (s1) {
            c = PTR(p, CTX_SUFFIX(c));
            s = s1;
            s1 = NULL;
        } else {
            if (CTX_SUFFIX(c) == 0)
                return REF(p, c);
            c = PTR(p, CTX_SUFFIX(c));
            if (CTX_NS(c) != 0) {
                s = PTR(p, CTX_STATS(c));
                if (ST_SYM(s) != f_sym)
                    do { s += 6; } while (ST_SYM(s) != f_sym);
                if (ST_FREQ(s) < MAX_FREQ - 9) {
                    s[1] = (uint8_t)(s[1] + 2);
                    wr16(c + 2, CTX_SUMM(c) + 2);
                }
            } else {
                s = ONE_STATE(c);
                if (ST_FREQ(s) < 32)
                    s[1]++;
            }
        }
        if (ST_SUCC(s) != 0)
            break;
        set_succ(s, up_branch);
        p->order_fall++;
    }
    if (walk_dbg)
        fprintf(stderr, "    RO stop c=0x%x s=0x%x sym=%u succ=0x%x of=%u\n",
                (unsigned)REF(p, c), (unsigned)REF(p, s), ST_SYM(s),
                (unsigned)ST_SUCC(s), p->order_fall);
    if (ST_SUCC(s) <= up_branch) {
        uint32_t successor;
        uint32_t saved = p->found_state;
        p->found_state = REF(p, s);
        successor = create_successors(p, 0, NULL, REF(p, c));
        if (successor == 0)
            set_succ(s, 0);
        else
            set_succ(s, successor);
        p->found_state = saved;
    }
    {
        uint32_t successor = ST_SUCC(s);
        if (p->order_fall == 1 && c1 == p->max_ctx) {
            set_succ(PTR(p, p->found_state), successor);
            p->text--;
        }
        if (successor == 0)
            return 0;
        return successor;
    }
}

static void update_model(struct sqx_ppm8 *p)
{
    uint32_t max_successor, min_successor = ST_SUCC(PTR(p, p->found_state));
    uint32_t c;
    unsigned s0, ns, f_freq = ST_FREQ(PTR(p, p->found_state));
    uint8_t flag;
    unsigned f_symbol = ST_SYM(PTR(p, p->found_state));
    uint8_t *s = NULL;

    if (f_freq < MAX_FREQ / 4 && CTX_SUFFIX(PTR(p, p->min_ctx)) != 0) {
        uint8_t *c2;
        c = CTX_SUFFIX(PTR(p, p->min_ctx));
        c2 = PTR(p, c);
        if (CTX_NS(c2) == 0) {
            s = ONE_STATE(c2);
            if (ST_FREQ(s) < 32)
                s[1]++;
        } else {
            s = PTR(p, CTX_STATS(c2));
            if (ST_SYM(s) != f_symbol) {
                do { s += 6; } while (ST_SYM(s) != f_symbol);
                if (ST_FREQ(s) >= ST_FREQ(s - 6)) {
                    uint8_t tmp[6];
                    memcpy(tmp, s, 6);
                    memcpy(s, s - 6, 6);
                    memcpy(s - 6, tmp, 6);
                    s -= 6;
                }
            }
            if (ST_FREQ(s) < MAX_FREQ - 9) {
                s[1] = (uint8_t)(s[1] + 2);
                wr16(c2 + 2, CTX_SUMM(c2) + 2);
            }
        }
    }
    c = p->max_ctx;
    if (p->order_fall == 0 && min_successor != 0) {
        uint32_t cs = create_successors(p, 1, s, p->min_ctx);
        if (cs == 0) {
            set_succ(PTR(p, p->found_state), 0);
            restore_model(p, c);
            return;
        }
        set_succ(PTR(p, p->found_state), cs);
        p->min_ctx = p->max_ctx = cs;
        return;
    }
    {
        uint32_t text = p->text;
        PTR(p, text)[0] = (uint8_t)f_symbol;
        text++;
        p->text = text;
        if (text >= p->units_start) {
            restore_model(p, c);
            return;
        }
        max_successor = text;
    }
    if (walk_dbg)
        fprintf(stderr, "  UM found=0x%x sym=%u freq=%u of=%u min_succ=0x%x max_ctx=0x%x"
                " min_ctx=0x%x s1=%s text=0x%x units_start=0x%x\n",
                (unsigned)p->found_state, f_symbol, f_freq, p->order_fall,
                (unsigned)min_successor, (unsigned)p->max_ctx, (unsigned)p->min_ctx,
                s ? "set" : "NULL", (unsigned)p->text, (unsigned)p->units_start);
    if (min_successor == 0) {
        uint32_t cs = reduce_order(p, s, p->min_ctx);
        if (cs == 0) {
            restore_model(p, c);
            return;
        }
        min_successor = cs;
    } else if (min_successor < p->units_start) {
        uint32_t cs = create_successors(p, 0, s, p->min_ctx);
        if (cs == 0) {
            restore_model(p, c);
            return;
        }
        min_successor = cs;
    }
    if (--p->order_fall == 0) {
        max_successor = min_successor;
        if (p->max_ctx != p->min_ctx)
            p->text--;
    }

    flag = (uint8_t)(f_symbol >= 0x40 ? 0x08u : 0u);
    s0 = CTX_SUMM(PTR(p, p->min_ctx)) - (ns = CTX_NS(PTR(p, p->min_ctx))) - f_freq;

    for (; c != p->min_ctx; c = CTX_SUFFIX(PTR(p, c))) {
        uint8_t *cc = PTR(p, c);
        unsigned ns1;
        uint32_t sum;
        if ((ns1 = CTX_NS(cc)) != 0) {
            if ((ns1 & 1) != 0) {
                unsigned old_nu = (ns1 + 1) >> 1;
                unsigned i = u2i(p, old_nu);
                if (i != u2i(p, old_nu + 1)) {
                    uint8_t *ptr = alloc_units(p, i + 1);
                    uint8_t *old_ptr;
                    if (!ptr) {
                        restore_model(p, min_successor);
                        return;
                    }
                    old_ptr = PTR(p, CTX_STATS(cc));
                    mem12(ptr, old_ptr, old_nu);
                    insert_node(p, old_ptr, i);
                    wr32(cc + 4, REF(p, ptr));
                }
            }
            sum = CTX_SUMM(cc);
            if (3 * (uint32_t)ns1 + 1 < ns)
                sum += 1;
        } else {
            uint8_t *st = alloc_units(p, 0);
            if (!st) {
                restore_model(p, min_successor);
                return;
            }
            {
                unsigned freq = ONE_STATE(cc)[1];
                st[0] = ONE_STATE(cc)[0];
                memcpy(st + 2, cc + 4, 4);
                wr32(cc + 4, REF(p, st));
                if (freq < MAX_FREQ / 4 - 1)
                    freq <<= 1;
                else
                    freq = MAX_FREQ - 4;
                st[1] = (uint8_t)freq;
                sum = freq + p->init_esc + (ns > 2 ? 1u : 0u);
            }
        }
        {
            uint8_t *st = PTR(p, CTX_STATS(cc)) + (size_t)(ns1 + 1) * 6;
            uint32_t cf = 2 * (sum + 6) * (uint32_t)f_freq;
            uint32_t sf = (uint32_t)s0 + sum;
            st[0] = (uint8_t)f_symbol;
            cc[0] = (uint8_t)(ns1 + 1);
            set_succ(st, max_successor);
            cc[1] = (uint8_t)(cc[1] | flag);
            if (cf < 6 * sf) {
                cf = 1 + (cf > sf ? 1u : 0u) + (cf >= 4 * sf ? 1u : 0u);
                sum += 4;
            } else {
                cf = 4 + (cf > 9 * sf ? 1u : 0u) + (cf > 12 * sf ? 1u : 0u)
                       + (cf > 15 * sf ? 1u : 0u);
                sum += cf;
            }
            wr16(cc + 2, sum);
            st[1] = (uint8_t)cf;
        }
    }
    p->max_ctx = p->min_ctx = min_successor;
}

static void rescale(struct sqx_ppm8 *p)
{
    unsigned i, adder, sum_freq, esc_freq;
    uint32_t mc = p->min_ctx;
    uint8_t *stats = PTR(p, CTX_STATS(PTR(p, mc)));
    uint8_t *s = PTR(p, p->found_state);

    if (s != stats) {
        uint8_t tmp[6];
        memcpy(tmp, s, 6);
        do {
            memcpy(s, s - 6, 6);
            s -= 6;
        } while (s != stats);
        memcpy(s, tmp, 6);
    }
    sum_freq = ST_FREQ(s);
    esc_freq = CTX_SUMM(PTR(p, mc)) - sum_freq;
    adder = (p->order_fall != 0) ? 1u : 0u;
    sum_freq = (sum_freq + 4 + adder) >> 1;
    i = CTX_NS(PTR(p, mc));
    s[1] = (uint8_t)sum_freq;
    do {
        unsigned freq;
        s += 6;
        freq = ST_FREQ(s);
        esc_freq -= freq;
        freq = (freq + adder) >> 1;
        sum_freq += freq;
        s[1] = (uint8_t)freq;
        if (freq > ST_FREQ(s - 6)) {
            uint8_t tmp[6];
            uint8_t *s1 = s;
            memcpy(tmp, s, 6);
            do {
                memcpy(s1, s1 - 6, 6);
                s1 -= 6;
            } while (s1 != stats && freq > ST_FREQ(s1 - 6));
            memcpy(s1, tmp, 6);
        }
    } while (--i);
    if (ST_FREQ(s) == 0) {
        unsigned num_stats, num_stats_new, n0, n1;
        i = 0;
        do { i++; } while (ST_FREQ(s -= 6) == 0);
        esc_freq += i;
        num_stats = CTX_NS(PTR(p, mc));
        num_stats_new = num_stats - i;
        PTR(p, mc)[0] = (uint8_t)num_stats_new;
        n0 = (num_stats + 2) >> 1;
        if (num_stats_new == 0) {
            unsigned freq = (2 * ST_FREQ(stats) + esc_freq - 1) / esc_freq;
            if (freq > MAX_FREQ / 3)
                freq = MAX_FREQ / 3;
            PTR(p, mc)[1] = (uint8_t)((CTX_FLAGS(PTR(p, mc)) & FLAG_PREV_HIGH)
                                      + (ST_SYM(stats) >= 0x40 ? 0x08u : 0u));
            s = ONE_STATE(PTR(p, mc));
            memcpy(s, stats, 6);
            s[1] = (uint8_t)freq;
            p->found_state = REF(p, s);
            insert_node(p, stats, u2i(p, n0));
            return;
        }
        n1 = (num_stats_new + 2) >> 1;
        if (n0 != n1)
            wr32(PTR(p, mc) + 4, REF(p, shrink_units(p, stats, n0, n1)));
    }
    wr16(PTR(p, mc) + 2,
         (uint32_t)(uint16_t)(sum_freq + esc_freq - (esc_freq >> 1)));
    PTR(p, mc)[1] |= (uint8_t)FLAG_RESCALED;
    p->found_state = CTX_STATS(PTR(p, mc));
}

static void next_context(struct sqx_ppm8 *p)
{
    uint32_t c = ST_SUCC(PTR(p, p->found_state));
    if (p->order_fall == 0 && c >= p->units_start)
        p->max_ctx = p->min_ctx = c;
    else
        update_model(p);
}

static void update1(struct sqx_ppm8 *p)
{
    uint8_t *s = PTR(p, p->found_state);
    unsigned freq = ST_FREQ(s) + 4;
    uint32_t mc = p->min_ctx;

    wr16(PTR(p, mc) + 2, CTX_SUMM(PTR(p, mc)) + 4);
    s[1] = (uint8_t)freq;
    if (freq > ST_FREQ(s - 6)) {
        uint8_t tmp[6];
        memcpy(tmp, s, 6);
        memcpy(s, s - 6, 6);
        memcpy(s - 6, tmp, 6);
        s -= 6;
        p->found_state = REF(p, s);
        if (freq > MAX_FREQ)
            rescale(p);
    }
    next_context(p);
}

static void update1_0(struct sqx_ppm8 *p)
{
    uint8_t *s = PTR(p, p->found_state);
    uint32_t mc = p->min_ctx;
    unsigned freq = ST_FREQ(s);
    unsigned summ_freq = CTX_SUMM(PTR(p, mc));

    p->prev_success = (2 * freq >= summ_freq) ? 1u : 0u;
    p->run_length += (int32_t)p->prev_success;
    wr16(PTR(p, mc) + 2, summ_freq + 4);
    freq += 4;
    s[1] = (uint8_t)freq;
    if (freq > MAX_FREQ)
        rescale(p);
    next_context(p);
}

static void update2(struct sqx_ppm8 *p)
{
    uint8_t *s = PTR(p, p->found_state);
    unsigned freq = ST_FREQ(s) + 4;
    uint32_t mc = p->min_ctx;

    p->run_length = p->init_rl;
    wr16(PTR(p, mc) + 2, CTX_SUMM(PTR(p, mc)) + 4);
    s[1] = (uint8_t)freq;
    if (freq > MAX_FREQ)
        rescale(p);
    update_model(p);
}

/* BinSumm[NS2Indx[OneState->Freq - 1]][PrevSuccess + ((RunLength>>26)&0x20)
 *        + NS2BSIndx[Suffix->NumStats] + Flags] */
static uint16_t *get_bin_summ(struct sqx_ppm8 *p)
{
    uint8_t *mc = PTR(p, p->min_ctx);
    unsigned freq = ST_FREQ(ONE_STATE(mc));
    unsigned suffix_ns = CTX_NS(PTR(p, CTX_SUFFIX(mc)));
    unsigned row = (unsigned)p->ns2indx[freq ? freq - 1 : 0];
    unsigned col = p->prev_success
                   + (((uint32_t)p->run_length >> 26) & 0x20u)
                   + (unsigned)p->ns2bsindx[suffix_ns]
                   + CTX_FLAGS(mc);
    if (row > 24)
        row = 24;
    if (col > 63)
        col = 63;
    return &p->binsumm[row][col];
}

static uint32_t cutoff(struct sqx_ppm8 *p, uint32_t ctx, unsigned order)
{
    uint8_t *c = PTR(p, ctx);
    int ns = (int)CTX_NS(c);
    uint32_t nu;
    uint8_t *stats;

    if (ns == 0) {
        uint8_t *s = ONE_STATE(c);
        uint32_t successor = ST_SUCC(s);
        if (successor >= p->units_start) {
            if (order < p->max_order)
                successor = cutoff(p, successor, order + 1);
            else
                successor = 0;
            set_succ(s, successor);
            if (successor != 0 || order <= 9)
                return ctx;
        }
        special_free_unit(p, c);
        return 0;
    }
    nu = ((uint32_t)ns + 2) >> 1;
    {
        unsigned indx = u2i(p, nu);
        stats = PTR(p, CTX_STATS(c));
        if ((uint32_t)(stats - PTR(p, p->units_start)) <= (1u << 14)
            && CTX_STATS(c) <= p->freelist[indx]) {
            uint8_t *ptr = remove_node(p, indx);
            wr32(c + 4, REF(p, ptr));
            mem12(ptr, stats, nu);
            if (stats != PTR(p, p->units_start))
                insert_node(p, stats, indx);
            else
                p->units_start += u2b(i2u(p, indx));
            stats = ptr;
        }
    }
    {
        uint8_t *s = stats + (size_t)ns * 6;
        do {
            uint32_t successor = ST_SUCC(s);
            if (successor < p->units_start) {
                uint8_t *s2 = stats + (size_t)ns * 6;
                ns--;
                if (order) {
                    if (s != s2)
                        memcpy(s, s2, 6);
                } else {
                    uint8_t tmp[6];
                    memcpy(tmp, s, 6);
                    memcpy(s, s2, 6);
                    memcpy(s2, tmp, 6);
                    set_succ(s2, 0);
                }
            } else if (order < p->max_order) {
                set_succ(s, cutoff(p, successor, order + 1));
            } else {
                set_succ(s, 0);
            }
            s -= 6;
        } while (s >= stats);
    }
    if ((unsigned)ns != CTX_NS(c) && order) {
        if (ns < 0) {
            free_units(p, stats, nu);
            special_free_unit(p, c);
            return 0;
        }
        c[0] = (uint8_t)ns;
        if (ns == 0) {
            uint8_t *st = stats;
            c[1] = (uint8_t)((CTX_FLAGS(c) & FLAG_PREV_HIGH)
                             + (ST_SYM(st) >= 0x40 ? 0x08u : 0u));
            c[2] = st[0];
            c[3] = (uint8_t)((ST_FREQ(st) + 11) >> 3);
            wr32(c + 4, rd32(st + 2));
            free_units(p, stats, nu);
        } else {
            refresh(p, ctx, nu, CTX_SUMM(c) > 16 * (unsigned)ns ? 1u : 0u);
        }
    }
    return ctx;
}

/* -------------------------------------------------------- Symbol decoding - */

static int decode_symbol(struct sqx_ppm8 *p)
{
    uint32_t mc = p->min_ctx;
    uint8_t *mcp = PTR(p, mc);

    if (CTX_NS(mcp) != 0) {
        uint8_t *s = PTR(p, CTX_STATS(mcp));
        unsigned i;
        uint32_t count, hi_cnt;
        uint32_t summ_freq = CTX_SUMM(mcp);

        count = rc_threshold(p, summ_freq);
        hi_cnt = count;
        if ((int32_t)(count -= ST_FREQ(s)) < 0) {
            unsigned sym;
            rc_decode(p, 0, ST_FREQ(s));
            rc_norm(p);
            p->found_state = REF(p, s);
            sym = ST_SYM(s);
            update1_0(p);
            return (int)sym;
        }
        p->prev_success = 0;
        i = CTX_NS(mcp);
        do {
            s += 6;
            if ((int32_t)(count -= ST_FREQ(s)) < 0) {
                unsigned sym;
                rc_decode(p, (hi_cnt - count) - ST_FREQ(s), ST_FREQ(s));
                rc_norm(p);
                p->found_state = REF(p, s);
                sym = ST_SYM(s);
                update1(p);
                return (int)sym;
            }
        } while (--i);
        if (hi_cnt >= summ_freq)
            return -2;
        hi_cnt -= count;
        rc_decode(p, hi_cnt, summ_freq - hi_cnt);
        /* all symbols in current context are "already seen" */
        memset(p->mask, 0xFF, sizeof(p->mask));
        p->mask[ST_SYM(s)] = 0;
        {
            uint8_t *q = PTR(p, CTX_STATS(mcp));
            do {
                unsigned sym0 = ST_SYM(q);
                unsigned sym1 = ST_SYM(q + 6);
                q += 12;
                p->mask[sym0] = 0;
                p->mask[sym1] = 0;
            } while (q < s);
        }
        p->num_masked = CTX_NS(mcp);
        p->found_state = 0;
    } else {
        uint8_t *s = ONE_STATE(mcp);
        uint16_t *prob = get_bin_summ(p);
        uint32_t pr_old = *prob;
        if (getenv("SQX_PPM8_DEBUG"))
            fprintf(stderr, "   C 1state sym=%u freq=%u flags=0x%x summ=%u sufx_ns=%u"
                    " prev=%u rl=%d row=%d col=%d pr=%u rel=%08x q14=%08x\n",
                    ST_SYM(s), ST_FREQ(s), CTX_FLAGS(mcp), CTX_SUMM(mcp),
                    CTX_NS(PTR(p, CTX_SUFFIX(mcp))), p->prev_success,
                    (int)p->run_length, (int)((prob - &p->binsumm[0][0]) / 64),
                    (int)((prob - &p->binsumm[0][0]) % 64), pr_old,
                    p->rc_code - p->rc_low, p->rc_range >> 14);
        uint32_t q14 = p->rc_range >> 14;
        uint32_t size0 = q14 * pr_old;
        /* PPMD_UPDATE_PROB_1 : mean = (prob + (1 << (7-2))) >> 7 */
        uint32_t pr_new = pr_old
                          - ((pr_old + (1u << (PPMD_PERIOD_BITS - 2)))
                             >> PPMD_PERIOD_BITS);

        if (p->rc_code - p->rc_low < size0) {
            unsigned sym = ST_SYM(s);
            unsigned freq = ST_FREQ(s);
            uint32_t c = ST_SUCC(s);
            *prob = (uint16_t)(pr_new + (1u << PPMD_INT_BITS));
            p->rc_range = size0;
            rc_norm(p);
            p->found_state = REF(p, s);
            p->prev_success = 1;
            p->run_length++;
            s[1] = (uint8_t)(freq + (freq < 196 ? 1u : 0u));
            if (p->order_fall == 0 && c >= p->units_start)
                p->max_ctx = p->min_ctx = c;
            else
                update_model(p);
            return (int)sym;
        }
        *prob = (uint16_t)pr_new;
        p->init_esc = p->exp_escape[pr_new >> 10];
        p->rc_low += size0;
        p->rc_range = (PPMD_BIN_SCALE - pr_old) * q14;
        p->prev_success = 0;
        memset(p->mask, 0xFF, sizeof(p->mask));
        p->mask[ST_SYM(s)] = 0;
        p->num_masked = 0;
        p->found_state = 0;
    }

    /* Escape loop */
    for (;;) {
        uint8_t *s;
        uint32_t freq_sum, count, hi_cnt, freq_sum2;
        ppm_see *see;
        uint32_t mcp2;
        unsigned num_masked;

        rc_norm(p);
        mcp2 = p->min_ctx;
        num_masked = CTX_NS(PTR(p, mcp2));
        do {
            p->order_fall++;
            if (CTX_SUFFIX(PTR(p, mcp2)) == 0)
                return -1;
            mcp2 = CTX_SUFFIX(PTR(p, mcp2));
        } while (CTX_NS(PTR(p, mcp2)) == num_masked);
        p->min_ctx = mcp2;

        /* Only count the first (NumStats - numMasked) unmasked symbols
         * of the lower context */
        s = PTR(p, CTX_STATS(PTR(p, mcp2)));
        {
            unsigned ns2 = CTX_NS(PTR(p, mcp2));
            unsigned want = ns2 > p->num_masked ? ns2 - p->num_masked : 0;
            unsigned pos = 0, limit = ns2 + 1;
            uint8_t *st = s;
            hi_cnt = 0;
            p->n_low = 0;
            while (pos < limit && want > 0) {
                if (p->mask[ST_SYM(st)] != 0) {
                    p->low_states[p->n_low++] = st;
                    hi_cnt += ST_FREQ(st);
                    want--;
                }
                st += 6;
                pos++;
            }
        }
        see = make_esc_freq(p, num_masked, &freq_sum);
        freq_sum += hi_cnt;
        freq_sum2 = freq_sum;
        if (freq_sum2 > p->rc_range)
            freq_sum2 = p->rc_range;
        count = rc_threshold(p, freq_sum2);
        if (count < hi_cnt) {
            unsigned sym;
            unsigned k = 0;
            hi_cnt = count;
            while (k < p->n_low) {
                uint8_t *st = p->low_states[k++];
                count -= ST_FREQ(st);
                if ((int32_t)count < 0) {
                    s = st;
                    break;
                }
            }
            rc_decode(p, (hi_cnt - count) - ST_FREQ(s), ST_FREQ(s));
            rc_norm(p);
            see_update(see);
            p->found_state = REF(p, s);
            sym = ST_SYM(s);
            update2(p);
            return (int)sym;
        }
        if (count >= freq_sum2)
            return -2;
        rc_decode(p, hi_cnt, freq_sum2 - hi_cnt);
        see->summ = (uint16_t)(see->summ + freq_sum);
        for (unsigned j = 0; j < p->n_low; j++)
            p->mask[ST_SYM(p->low_states[j])] = 0;
        p->num_masked = CTX_NS(PTR(p, mcp2));
    }
}

/* Context chain fingerprint (diagnostic): ns, flags, SummFreq and
 * pairs of (symbol, freq), without references. */
static uint32_t chain_hash(struct sqx_ppm8 *p)
{
    uint32_t h = 2166136261u, c = p->min_ctx;
    int k = 0;
    while (c != 0 && k < 12) {
        uint8_t *q = PTR(p, c);
        unsigned ns = CTX_NS(q), i;
        h = (h ^ CTX_NS(q)) * 16777619u;
        h = (h ^ CTX_FLAGS(q)) * 16777619u;
        h = (h ^ CTX_SUMM(q)) * 16777619u;
        if (ns) {
            uint8_t *st = PTR(p, CTX_STATS(q));
            for (i = 0; i < ns + 1; i++, st += 6) {
                h = (h ^ ST_SYM(st)) * 16777619u;
                h = (h ^ ST_FREQ(st)) * 16777619u;
            }
        } else {
            h = (h ^ (unsigned)q[2]) * 16777619u;
            h = (h ^ (unsigned)q[3]) * 16777619u;
        }
        c = CTX_SUFFIX(q);
        k++;
    }
    return h;
}

/* --------------------------------------------------------- symbol encoding */

/* Range encoder is an exact mirror of the decoder above (and Ppmd8Enc.c
 * in 7-Zip): same normalization thresholds as rcoder.c (RC_TOP/RC_BOT),
 * same output order (high byte of `low`), and 4 bytes of `low` flushed at
 * end. Carryless normalization guarantees identical bitstream to SQX. */

static void rc_enc_byte(struct sqx_ppm8 *p)
{
    if (p->enc)
        sqx_bitw_put(p->enc, p->rc_low >> 24, 8);
    p->rc_range <<= 8;
    p->rc_low <<= 8;
}

static void rc_enc_norm(struct sqx_ppm8 *p)
{
    for (;;) {
        if ((p->rc_low ^ (p->rc_low + p->rc_range)) >= RC_TOP) {
            if (p->rc_range >= RC_BOT)
                break;
            p->rc_range = (0u - p->rc_low) & (RC_BOT - 1u);
        }
        rc_enc_byte(p);
    }
}

/* low += start * (range /= total) ; range *= size (sans normalisation) */
static void rc_enc(struct sqx_ppm8 *p, uint32_t start, uint32_t size,
                   uint32_t total)
{
    if (total == 0) {
        p->rc_range = 0;
        return;
    }
    p->rc_range /= total;
    p->rc_low += start * p->rc_range;
    p->rc_range *= size;
}

/* Mirror of rc_get_bits: n uniform bits written MSB to LSB */
static void rc_enc_bits(struct sqx_ppm8 *p, unsigned n, uint32_t v)
{
    while (n--) {
        uint32_t r = p->rc_range >> 1;
        p->rc_range = r;
        if ((v >> n) & 1u)
            p->rc_low += r;
        rc_enc_norm(p);
    }
}

static void rc_enc_flush(struct sqx_ppm8 *p)
{
    unsigned i;
    for (i = 0; i < 4; i++) {
        if (p->enc)
            sqx_bitw_put(p->enc, p->rc_low >> 24, 8);
        p->rc_low <<= 8;
    }
}

static int encode_symbol(struct sqx_ppm8 *p, unsigned sym)
{
    uint32_t mc = p->min_ctx;
    uint8_t *mcp = PTR(p, mc);

    if (CTX_NS(mcp) != 0) {
        uint8_t *s = PTR(p, CTX_STATS(mcp));
        unsigned i;
        uint32_t sum;
        uint32_t summ_freq = CTX_SUMM(mcp);

        if (ST_SYM(s) == sym) {
            rc_enc(p, 0, ST_FREQ(s), summ_freq);
            rc_enc_norm(p);
            p->found_state = REF(p, s);
            update1_0(p);
            return 1;
        }
        p->prev_success = 0;
        sum = ST_FREQ(s);
        i = CTX_NS(mcp);
        do {
            s += 6;
            if (ST_SYM(s) == sym) {
                rc_enc(p, sum, ST_FREQ(s), summ_freq);
                rc_enc_norm(p);
                p->found_state = REF(p, s);
                update1(p);
                return 1;
            }
            sum += ST_FREQ(s);
        } while (--i);
        rc_enc(p, sum, summ_freq - sum, summ_freq);
        /* symbol absent: all symbols in current context are "already seen" */
        memset(p->mask, 0xFF, sizeof(p->mask));
        p->mask[ST_SYM(s)] = 0;
        {
            uint8_t *q = PTR(p, CTX_STATS(mcp));
            do {
                unsigned sym0 = ST_SYM(q);
                unsigned sym1 = ST_SYM(q + 6);
                q += 12;
                p->mask[sym0] = 0;
                p->mask[sym1] = 0;
            } while (q < s);
        }
        p->num_masked = CTX_NS(mcp);
        p->found_state = 0;
    } else {
        uint8_t *s = ONE_STATE(mcp);
        uint16_t *prob = get_bin_summ(p);
        uint32_t pr_old = *prob;
        uint32_t q14 = p->rc_range >> 14;
        uint32_t size0 = q14 * pr_old;
        uint32_t pr_new = pr_old
                          - ((pr_old + (1u << (PPMD_PERIOD_BITS - 2)))
                             >> PPMD_PERIOD_BITS);

        if (ST_SYM(s) == sym) {
            unsigned freq = ST_FREQ(s);
            uint32_t c = ST_SUCC(s);
            *prob = (uint16_t)(pr_new + (1u << PPMD_INT_BITS));
            p->rc_range = size0;
            rc_enc_norm(p);
            p->found_state = REF(p, s);
            p->prev_success = 1;
            p->run_length++;
            s[1] = (uint8_t)(freq + (freq < 196 ? 1u : 0u));
            if (p->order_fall == 0 && c >= p->units_start)
                p->max_ctx = p->min_ctx = c;
            else
                update_model(p);
            return 1;
        }
        *prob = (uint16_t)pr_new;
        p->init_esc = p->exp_escape[pr_new >> 10];
        p->rc_low += size0;
        p->rc_range = (PPMD_BIN_SCALE - pr_old) * q14;
        p->prev_success = 0;
        memset(p->mask, 0xFF, sizeof(p->mask));
        p->mask[ST_SYM(s)] = 0;
        p->num_masked = 0;
        p->found_state = 0;
    }

    /* Escape loop: mirror of decode_symbol (same lower contexts,
     * same counted symbols set, same order) */
    for (;;) {
        uint8_t *s, *st, *found = NULL;
        uint32_t low = 0, un_sum = 0, total, esc_freq;
        ppm_see *see;
        uint32_t mcp2;
        unsigned num_masked, ns2, want, k, limit, left;

        rc_enc_norm(p);
        mcp2 = p->min_ctx;
        num_masked = CTX_NS(PTR(p, mcp2));
        do {
            p->order_fall++;
            if (CTX_SUFFIX(PTR(p, mcp2)) == 0)
                return 0;                 /* end of stream: illegal symbol */
            mcp2 = CTX_SUFFIX(PTR(p, mcp2));
        } while (CTX_NS(PTR(p, mcp2)) == num_masked);
        p->min_ctx = mcp2;

        ns2 = CTX_NS(PTR(p, mcp2));
        want = ns2 > p->num_masked ? ns2 - p->num_masked : 0;
        limit = ns2 + 1;
        s = PTR(p, CTX_STATS(PTR(p, mcp2)));
        for (k = 0, st = s; k < limit && want > 0; k++, st += 6) {
            if (p->mask[ST_SYM(st)] == 0)
                continue;
            if (ST_SYM(st) == sym) {
                low = un_sum;
                found = st;
            }
            un_sum += ST_FREQ(st);
            want--;
        }
        if (!found) {
            /* Symbol must be unmasked here: otherwise codec state
             * is inconsistent (corrupted input stream) */
            for (k = 0, st = s; k < limit; k++, st += 6) {
                if (p->mask[ST_SYM(st)] != 0 && ST_SYM(st) == sym)
                    return 0;
            }
        }
        see = make_esc_freq(p, num_masked, &esc_freq);
        total = un_sum + esc_freq;
        if (total > p->rc_range)
            total = p->rc_range;

        if (found) {
            see_update(see);
            p->found_state = REF(p, found);
            rc_enc(p, low, ST_FREQ(found), total);
            rc_enc_norm(p);
            update2(p);
            return 1;
        }
        see->summ = (uint16_t)(see->summ + un_sum + esc_freq);
        rc_enc(p, un_sum, total - un_sum, total);
        /* mask symbols from lower context already counted (same states as decoder) */
        left = ns2 > p->num_masked ? ns2 - p->num_masked : 0;
        for (k = 0, st = s; k < limit && left > 0; k++, st += 6) {
            if (p->mask[ST_SYM(st)] == 0)
                continue;
            p->mask[ST_SYM(st)] = 0;
            left--;
        }
        p->num_masked = ns2;
    }
}

/* ------------------------------------------------------------------- API --- */

static void construct(struct sqx_ppm8 *p)
{
    unsigned i, k, m;

    for (i = 0, k = 0; i < PPMD_NUM_INDEXES; i++) {
        unsigned step = (i >= 12 ? 4 : (i >> 2) + 1);
        do {
            p->units2indx[k++] = (uint8_t)i;
        } while (--step);
        p->indx2units[i] = (uint8_t)k;
    }
    p->ns2bsindx[0] = (uint8_t)(0 << 1);
    p->ns2bsindx[1] = (uint8_t)(1 << 1);
    memset(p->ns2bsindx + 2, (2 << 1), 9);
    memset(p->ns2bsindx + 11, (3 << 1), 256 - 11);
    for (i = 0; i < 5; i++)
        p->ns2indx[i] = (uint8_t)i;
    for (m = i, k = 1; i < 260; i++) {
        p->ns2indx[i] = (uint8_t)m;
        if (--k == 0)
            k = (++m) - 4;
    }
    memcpy(p->exp_escape, k_exp_escape, 16);
}

sqx_ppm8 *sqx_ppm8_new(void)
{
    struct sqx_ppm8 *p = (struct sqx_ppm8 *)malloc(sizeof(*p));
    if (!p)
        return NULL;
    memset(p, 0, sizeof(*p));
    construct(p);
    return p;
}

void sqx_ppm8_free(sqx_ppm8 *z)
{
    if (!z)
        return;
    free(z->base);
    free(z);
}

static int arena_alloc(struct sqx_ppm8 *p, uint32_t size)
{
    if (p->base && p->size == size)
        return 1;
    free(p->base);
    p->base = NULL;
    p->size = 0;
    p->align_offset = (4 - size) & 3;
    p->base = (uint8_t *)malloc((size_t)p->align_offset + size);
    if (!p->base)
        return 0;
    p->size = size;
    return 1;
}

int sqx_ppm8_decode(sqx_ppm8 *z, const uint8_t *src, size_t src_len,
                    uint64_t orig_size, uint8_t *dst, size_t dst_cap)
{
    struct sqx_ppm8 *p = z;
    uint64_t i;
    unsigned cont, order, mem;

    if (orig_size > dst_cap)
        return 0;
    sqx_bits_init(&p->bits, src, src_len);
    p->rc_low = 0;
    p->rc_code = 0;
    p->rc_range = 0xFFFFFFFFu;
    for (i = 0; i < 4; i++)
        p->rc_code = (p->rc_code << 8) | sqx_bits_get(&p->bits, 8);

    cont = rc_get_bits(p, 1);
    if (getenv("SQX_PPM8_DEBUG"))
        fprintf(stderr, "ppm8: cont=%u%s", (unsigned)cont,
                cont ? "\n" : " ");
    if (!cont) {
        order = rc_get_bits(p, 8);
        mem = rc_get_bits(p, 10);
        if (getenv("SQX_PPM8_DEBUG"))
            fprintf(stderr, "order=%u mem=%u MB\n", (unsigned)order, (unsigned)mem);
        if (order < PPMD8_MIN_ORDER || order > PPMD8_MAX_ORDER)
            return 0;
        if (mem == 0)
            return 0;
        if (!arena_alloc(p, mem << 20))
            return 0;
        model_init(p, order, 0);
    } else if (!p->have_model) {
        return 0;
    }

    for (i = 0; i < orig_size; i++) {
        int sym;
        if (getenv("SQX_PPM8_DEBUG_WALK"))
            fprintf(stderr, "# symbol %llu\n", (unsigned long long)i);
        if (getenv("SQX_PPM8_DEBUG"))
            fprintf(stderr, "[%llu] ns=%u flags=0x%x summ=%u of=%u range=%08x h=%08x\n",
                    (unsigned long long)i, CTX_NS(PTR(p, p->min_ctx)),
                    CTX_FLAGS(PTR(p, p->min_ctx)), CTX_SUMM(PTR(p, p->min_ctx)),
                    p->order_fall, p->rc_range, chain_hash(p));
        if (getenv("SQX_PPM8_DEBUG_CHAIN")) {
            uint32_t c = p->min_ctx;
            int k = 0;
            while (c != 0 && k < 6) {
                uint8_t *q = PTR(p, c);
                unsigned ns = CTX_NS(q);
                fprintf(stderr, "   C[%llu].%d ref=0x%x ns=%u flags=0x%x summ=%u",
                        (unsigned long long)i, k, (unsigned)c, ns, CTX_FLAGS(q), CTX_SUMM(q));
                if (ns == 0)
                    fprintf(stderr, " sym=%u freq=%u", (unsigned)q[2], (unsigned)q[3]);
                fprintf(stderr, " prev=%u\n", p->prev_success);
                c = CTX_SUFFIX(q);
                k++;
            }
        }
        walk_dbg = (getenv("SQX_PPM8_DEBUG_WALK") != NULL);
        sym = decode_symbol(p);
        walk_dbg = 0;
        if (getenv("SQX_PPM8_DEBUG"))
            fprintf(stderr, "     -> %d\n", sym);
        if (sym < 0)
            return 0;
        dst[i] = (uint8_t)sym;
    }
    return 1;
}

int sqx_ppm8_encode(sqx_ppm8 *z, const uint8_t *src, size_t len,
                    unsigned order, unsigned mem_mb, int fresh,
                    uint8_t **out, size_t *out_len)
{
    struct sqx_ppm8 *p = z;
    sqx_bitw w;
    size_t i;

    *out = NULL;
    *out_len = 0;
    if (!p)
        return 0;
    if (fresh) {
        if (order == 0)
            order = 6;                 /* defaut de l'oracle (sqc -fmt sqx2 -m5) */
        if (mem_mb == 0)
            mem_mb = 32;               /* idem */
        if (order < PPMD8_MIN_ORDER || order > PPMD8_MAX_ORDER)
            return 0;
        if (mem_mb > 1023)
            return 0;
        if (!arena_alloc(p, mem_mb << 20))
            return 0;
        model_init(p, order, 0);
    } else if (!p->have_model) {
        return 0;                      /* rien a poursuivre */
    }

    sqx_bitw_init(&w, len + 64);
    if (w.err) {
        sqx_bitw_free(&w);
        return 0;
    }
    p->enc = &w;
    p->rc_low = 0;
    p->rc_range = 0xFFFFFFFFu;

    /* Stream header (same fields as read by sqx_ppm8_decode):
     * 1 = continuation of an already initialized model, else 0 + order (8 bits) +
     * memory in MB (10 bits) */
    if (fresh) {
        rc_enc_bits(p, 1, 0);
        rc_enc_bits(p, 8, order);
        rc_enc_bits(p, 10, mem_mb);
    } else {
        rc_enc_bits(p, 1, 1);
    }

    for (i = 0; i < len; i++) {
        if (!encode_symbol(p, src[i])) {
            p->enc = NULL;
            sqx_bitw_free(&w);
            return 0;
        }
    }
    rc_enc_flush(p);
    p->enc = NULL;
    sqx_bitw_flush(&w);
    if (w.err) {
        sqx_bitw_free(&w);
        return 0;
    }
    *out = w.buf;
    *out_len = w.len;
    return 1;
}
