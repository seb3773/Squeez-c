/* ultra.c — SQX Method 7 "Ultra" decoder (LZ77 + probabilistic range coder).
 *
 * Port of UnSqueez.exe sub_40D8B1 (main loop), sub_40D03E (model setup),
 * sub_40CF40 (reset), sub_4097FA (binary), sub_40A05E (Fenwick),
 * sub_4099AB (literal), sub_409BA3/sub_409C2A/sub_409C92 (range core).
 */
#include "ultra.h"
#include "bitio.h"
#include "sqx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ULTRA_TOP 0x01000000u
#define ULTRA_BOT 0x00010000u

/* Length groups: base/next/alphabet/incr/max (dword_420D00, 9 structs). */
static const uint32_t LT_BASE[9] = { 2, 3, 4, 6, 10, 18, 34, 66, 130 };
static const uint32_t LT_NEXT[9] = { 0, 0, 2, 3, 4, 5, 6, 7, 8 };
static const uint32_t LT_NSYM[9] = { 0, 0, 2, 4, 8, 16, 32, 64, 128 };
static const uint32_t LT_INCR[9] = { 0, 0, 1, 1, 2, 2, 2, 4, 4 };
static const uint32_t LT_MAX[9]  = { 0, 0, 255, 255, 255, 255, 255, 255, 255 };

/* Distance base / extra bits (dword_420B70 / dword_420C38, 50 entries). */
static const uint32_t DT_BASE[50] = {
    0, 1, 2, 3, 4, 6, 8, 0xC, 0x10, 0x18, 0x20, 0x30, 0x40, 0x60,
    0x80, 0xC0, 0x100, 0x180, 0x200, 0x300, 0x400, 0x600, 0x800, 0xC00,
    0x1000, 0x1800, 0x2000, 0x3000, 0x4000, 0x6000, 0x8000, 0xC000,
    0x10000, 0x18000, 0x20000, 0x30000, 0x40000, 0x60000, 0x80000, 0xC0000,
    0x100000, 0x180000, 0x200000, 0x300000, 0x400000, 0x600000,
    0x800000, 0xC00000, 0x1000000, 0x1800000
};
static const uint32_t DT_EBITS[50] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16,
    17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 23
};

static int fm_init(ultra_fm *m, uint32_t nsym, uint32_t incr, uint32_t maxf)
{
    m->nsym = nsym;
    m->incr = incr;
    m->maxf = maxf;
    m->freq = NULL;
    m->tot = 0;
    if (nsym == 0)
        return 1;
    m->freq = (uint32_t *)malloc(sizeof(uint32_t) * nsym);
    if (!m->freq)
        return 0;
    m->tot = nsym * incr;
    for (uint32_t i = 0; i < nsym; i++)
        m->freq[i] = incr;
    return 1;
}

static void fm_reset(ultra_fm *m)
{
    if (!m->freq)
        return;
    m->tot = m->nsym * m->incr;
    for (uint32_t i = 0; i < m->nsym; i++)
        m->freq[i] = m->incr;
}

static void fm_free(ultra_fm *m)
{
    free(m->freq);
    m->freq = NULL;
    m->nsym = m->tot = 0;
}

static void bm_init(ultra_bm *m, uint32_t incr, uint32_t maxf)
{
    m->incr = incr;
    m->maxf = maxf;
    m->f0 = m->f1 = incr;
    m->tot = 2 * incr;
}

static void bm_reset(ultra_bm *m)
{
    m->f0 = m->f1 = m->incr;
    m->tot = 2 * m->incr;
}

int sqx_ultra_alloc(sqx_ultra *u)
{
    unsigned a, b, c, p, n;
    memset(u, 0, sizeof(*u));
    for (a = 0; a < 2; a++)
        for (b = 0; b < 256; b++)
            for (c = 0; c < 17; c++)
                if (!fm_init(&u->lit[a][b].sub[c], 16, a ? 2u : 1u, 511u))
                    return 0;
    for (a = 0; a < 4; a++)
        for (b = 0; b < 4; b++)
            for (c = 0; c < 4; c++)
                bm_init(&u->is_match[a][b][c], 8, 1023);
    for (a = 0; a < 4; a++)
        for (b = 0; b < 4; b++)
            bm_init(&u->is_rep[a][b], 8, 1023);
    for (a = 0; a < 4; a++)
        if (!fm_init(&u->len_dist[a], 50, 4, 1023))
            return 0;
    if (!fm_init(&u->dist_low, 16, 4, 1023))
        return 0;
    for (a = 0; a < 4; a++) {
        if (!fm_init(&u->len_new[a], 9, 8, 1023))
            return 0;
        if (!fm_init(&u->len_sub[a], 9, 8, 1023))
            return 0;
        if (!fm_init(&u->len_rep[a], 6, 4, 1023))
            return 0;
    }
    for (p = 0; p < 4; p++)
        for (n = 0; n < 9; n++) {
            if (LT_NSYM[n]) {
                if (!fm_init(&u->sec_n[p][n], LT_NSYM[n], LT_INCR[n], LT_MAX[n]))
                    return 0;
                if (!fm_init(&u->sec_r[p][n], LT_NSYM[n], LT_INCR[n], LT_MAX[n]))
                    return 0;
            }
        }
    u->models_ready = 1;
    return 1;
}

void sqx_ultra_free(sqx_ultra *u)
{
    unsigned a, b, c, p, n;
    for (a = 0; a < 2; a++)
        for (b = 0; b < 256; b++)
            for (c = 0; c < 17; c++)
                fm_free(&u->lit[a][b].sub[c]);
    for (a = 0; a < 4; a++)
        fm_free(&u->len_dist[a]);
    fm_free(&u->dist_low);
    for (a = 0; a < 4; a++) {
        fm_free(&u->len_new[a]);
        fm_free(&u->len_sub[a]);
        fm_free(&u->len_rep[a]);
    }
    for (p = 0; p < 4; p++)
        for (n = 0; n < 9; n++) {
            fm_free(&u->sec_n[p][n]);
            fm_free(&u->sec_r[p][n]);
        }
    free(u->window);
    free(u->out);
    memset(u, 0, sizeof(*u));
}

static void ultra_reset_models(sqx_ultra *u)
{
    unsigned a, b, c, p, n;
    for (a = 0; a < 2; a++)
        for (b = 0; b < 256; b++)
            for (c = 0; c < 17; c++)
                fm_reset(&u->lit[a][b].sub[c]);
    for (a = 0; a < 4; a++)
        for (b = 0; b < 4; b++) {
            for (c = 0; c < 4; c++)
                bm_reset(&u->is_match[a][b][c]);
            bm_reset(&u->is_rep[a][b]);
        }
    for (a = 0; a < 4; a++)
        fm_reset(&u->len_dist[a]);
    fm_reset(&u->dist_low);
    for (a = 0; a < 4; a++) {
        fm_reset(&u->len_new[a]);
        fm_reset(&u->len_sub[a]);
        fm_reset(&u->len_rep[a]);
    }
    for (p = 0; p < 4; p++)
        for (n = 0; n < 9; n++) {
            fm_reset(&u->sec_n[p][n]);
            fm_reset(&u->sec_r[p][n]);
        }
}

static void out_append(sqx_ultra *u, const uint8_t *p, size_t n)
{
    if (u->out_len + n > u->out_cap) {
        size_t cap = u->out_cap ? u->out_cap : 65536;
        while (cap < u->out_len + n)
            cap *= 2;
        u->out = (uint8_t *)realloc(u->out, cap);
        u->out_cap = cap;
    }
    memcpy(u->out + u->out_len, p, n);
    u->file_crc = sqx_crc32_raw(u->file_crc, p, n);
    u->out_len += n;
}

/* Flush window contents with wrap handling (mirrors sub_40D6C5). */
static void write_dictionary(sqx_ultra *u, uint32_t *write_pos, uint64_t *to_flush)
{
    uint32_t curr = u->curr_pos, wpos = *write_pos;
    if (*to_flush < 1)
        return;
    if (curr == u->dic_size && wpos == u->dic_size) {
        out_append(u, u->window, curr);
        *write_pos = 0;
        *to_flush -= u->dic_size;
    } else if (curr < wpos) {
        uint32_t n = (uint32_t)(-(int32_t)wpos) & (u->dic_size - 1);
        out_append(u, &u->window[wpos], n);
        *to_flush -= n;
        out_append(u, u->window, curr);
        *to_flush -= curr;
        *write_pos = curr;
    } else {
        out_append(u, &u->window[wpos], curr - wpos);
        *to_flush -= (curr - wpos);
        *write_pos = curr;
    }
}

/* Range normalizer shared by symbol and raw-bit paths. */
static int ultra_trace = -1;
static void ultra_fail(const char *where, uint64_t decoded, uint32_t state,
                       uint32_t prev_state, uint64_t remaining)
{
    if (ultra_trace < 0)
        ultra_trace = getenv("SQX_ULTRA_TRACE") ? 1 : 0;
    if (ultra_trace)
        fprintf(stderr, "[ultra] fail at %s decoded=%llu state=%u/%u remaining=%llu\n",
                where, (unsigned long long)decoded, state, prev_state,
                (unsigned long long)remaining);
}
#define UFAIL(w) do { ultra_fail(w, decoded, state, prev_state, remaining); return 0; } while (0)
static int rc_norm(sqx_ultra *u, sqx_bits *b)
{
    for (;;) {
        uint32_t lo = u->rc_low, rg = u->rc_range;
        if ((lo ^ (lo + rg)) < ULTRA_TOP) {
            /* read next byte */
        } else {
            if (rg >= ULTRA_BOT)
                return 1;
            u->rc_range = (uint32_t)(0u - lo) & (ULTRA_BOT - 1);
            /* fall through to read */
        }
        u->rc_code = (u->rc_code << 8) | sqx_bits_get(b, 8);
        u->rc_range <<= 8;
        u->rc_low <<= 8;
    }
}

static int bm_decode(sqx_ultra *u, ultra_bm *m, sqx_bits *b)
{
    uint32_t tot = m->tot, q, split, res;
    q = u->rc_range / tot;
    u->rc_range = q;
    if (q)
        split = (u->rc_code - u->rc_low) / q;
    else
        split = tot;
    if (m->f0 <= split) {
        if (m->f0 + m->f1 <= split)
            return -1;
        u->rc_low += m->f0 * u->rc_range;
        u->rc_range *= m->f1;
        m->f1 += m->incr;
        res = 1;
    } else {
        u->rc_low += 0;
        u->rc_range *= m->f0;
        m->f0 += m->incr;
        res = 0;
    }
    m->tot += m->incr;
    if (m->tot > m->maxf) {
        m->f0 -= (m->f0 >> 1);
        m->f1 -= (m->f1 >> 1);
        m->tot = m->f0 + m->f1;
    }
    if (!rc_norm(u, b))
        return -1;
    return (int)res;
}

static int fm_decode(sqx_ultra *u, ultra_fm *m, sqx_bits *b)
{
    uint32_t tot = m->tot, q, rf, sym, cum = 0;
    uint32_t i;
    q = u->rc_range / tot;
    u->rc_range = q;
    if (q)
        rf = (u->rc_code - u->rc_low) / q;
    else
        rf = tot;
    if (rf >= tot)
        return -1;
    for (sym = 0; sym < m->nsym; sym++) {
        if (cum + m->freq[sym] > rf)
            break;
        cum += m->freq[sym];
    }
    if (sym >= m->nsym)
        return -1;
    u->rc_low += cum * u->rc_range;
    u->rc_range *= m->freq[sym];
    m->freq[sym] += m->incr;
    m->tot += m->incr;
    if (m->tot > m->maxf) {
        uint32_t t = 0;
        for (i = 0; i < m->nsym; i++) {
            m->freq[i] -= (m->freq[i] >> 1);
            t += m->freq[i];
        }
        m->tot = t;
    }
    if (!rc_norm(u, b))
        return -1;
    return (int)sym;
}

static int rc_raw(sqx_ultra *u, sqx_bits *b, unsigned n)
{
    unsigned v = 0;
    while (n--) {
        uint32_t q;
        u->rc_range >>= 1;
        q = (u->rc_code - u->rc_low) / u->rc_range;
        v = (v << 1) | (q & 1u);
        u->rc_low += q * u->rc_range;
        if (!rc_norm(u, b))
            return -1;
    }
    return (int)v;
}

static int lit_decode(sqx_ultra *u, ultra_lit *lit, sqx_bits *b)
{
    int hi = fm_decode(u, &lit->sub[0], b);
    int lo;
    if (hi < 0)
        return -1;
    lo = fm_decode(u, &lit->sub[1 + hi], b);
    if (lo < 0)
        return -1;
    return hi * 16 + lo;
}

int sqx_ultra_decode(sqx_ultra *u, const uint8_t *src, size_t src_len,
                     uint64_t orig_size, int solid, uint32_t dic_size,
                     uint8_t comp_flags)
{
    sqx_bits b;
    uint64_t remaining = orig_size;
    uint64_t decoded = 0;
    uint32_t write_pos = 0;
    uint64_t to_flush = orig_size;
    uint32_t dist_mru[4] = { 0, 0, 0, 0 };
    uint32_t last_len = 0, last_dist_m1 = 0;
    uint32_t state = 0, prev_state = 0;
    uint32_t prev_byte = 0;
    uint32_t cur_dist = 1;
    unsigned i;

    if (!u->models_ready)
        return 0;
    if (dic_size == 0 || (dic_size & (dic_size - 1)) != 0)
        return 0;
    if (u->dic_size != dic_size || !u->window) {
        uint8_t *w = (uint8_t *)realloc(u->window, dic_size);
        if (!w && dic_size)
            return 0;
        u->window = w;
        u->dic_size = dic_size;
    }
    u->curr_pos = 0;
    u->out_len = 0;
    u->file_crc = SQX_CRC32_INIT;

    if (orig_size == 0)
        return 1;

    sqx_bits_init(&b, src, src_len);

    /* Extended compressor header (stored bytes, like LZH usExtCompressor).
     * Ultra samples use comp_flags == 0; anything else is not yet supported. */
    if (comp_flags != 0) {
        uint8_t hdr[2];
        hdr[0] = (uint8_t)sqx_bits_get(&b, 8);
        hdr[1] = (uint8_t)sqx_bits_get(&b, 8);
        (void)hdr;
        return 0;
    }

    if (!solid)
        ultra_reset_models(u);

    /* Range init: low = 0, code = 4 stream bytes (sub_409BA3). */
    u->rc_low = 0;
    u->rc_range = 0xFFFFFFFFu;
    u->rc_code = 0;
    for (i = 0; i < 4; i++)
        u->rc_code = (u->rc_code << 8) | sqx_bits_get(&b, 8);

    while (remaining > 0) {
        int is_m, new_state, n;
        uint32_t pos3 = (uint32_t)(decoded & 3u);

        is_m = bm_decode(u, &u->is_match[pos3][state][prev_state], &b);
        if (is_m < 0)
            return 0;
        if (is_m) {
            int is_r = bm_decode(u, &u->is_rep[state][prev_state], &b);
            if (is_r < 0)
                return 0;
            if (is_r) {
                int g = fm_decode(u, &u->len_rep[state], &b);
                if (g < 0)
                    return 0;
                if (g == 4) {           /* copy 1 byte at cur dist */
                    uint8_t byte;
                    if (cur_dist == 0 || cur_dist > u->dic_size)
                        UFAIL("copy1-dist");
                    byte = u->window[(u->curr_pos - cur_dist) & (u->dic_size - 1)];
                    u->window[u->curr_pos] = byte;
                    if (++u->curr_pos >= u->dic_size) {
                        write_dictionary(u, &write_pos, &to_flush);
                        u->curr_pos &= u->dic_size - 1;
                    }
                    /* NB: original sets prev = dict[(curr-dist)&mask]
                     * (next source byte), NOT the last output byte. */
                    prev_byte = u->window[(u->curr_pos - cur_dist) & (u->dic_size - 1)];
                    n = 1;
                    new_state = 3;
                } else if (g == 5) {    /* repeat last match */
                    uint32_t len = last_len, dist = last_dist_m1 + 1;
                    if (len == 0 || len > remaining)
                        UFAIL("rep5-len");
                    if (dist == 0 || dist > u->dic_size)
                        UFAIL("rep5-dist");
                    while (len--) {
                        u->window[u->curr_pos] =
                            u->window[(u->curr_pos - dist) & (u->dic_size - 1)];
                        if (++u->curr_pos >= u->dic_size) {
                            write_dictionary(u, &write_pos, &to_flush);
                            u->curr_pos &= u->dic_size - 1;
                        }
                    }
                    /* note: 2776/2780 keep last values (already set) */
                    cur_dist = dist;
                    n = (int)last_len;
                    new_state = 2;
                    prev_byte = u->window[(u->curr_pos - dist) & (u->dic_size - 1)];
                } else {                /* repeated offset k = g+1 */
                    uint32_t g2, base, extra = 0;
                    int e;
                    uint32_t len, dist_m1, dist;
                    if ((unsigned)g > 3)
                        return 0;
                    dist_m1 = dist_mru[g];
                    e = fm_decode(u, &u->len_sub[state], &b);
                    if (e < 0)
                        return 0;
                    g2 = (uint32_t)e;
                    if (g2 > 8)
                        return 0;
                    base = LT_BASE[g2];
                    if (LT_NEXT[g2]) {
                        e = fm_decode(u, &u->sec_r[state][LT_NEXT[g2]], &b);
                        if (e < 0)
                            return 0;
                        extra = (uint32_t)e;
                    }
                    len = base + extra;
                    if (len == 257) {
                        e = rc_raw(u, &b, 14);
                        if (e < 0)
                            return 0;
                        len = 257u + (uint32_t)e;
                    }
                    if (len == 0 || len > remaining)
                        return 0;
                    dist = dist_m1 + 1;
                    if (dist == 0 || dist > u->dic_size)
                        return 0;
                    /* MRU update */
                    for (i = (unsigned)g; i > 0; i--)
                        dist_mru[i] = dist_mru[i - 1];
                    dist_mru[0] = dist_m1;
                    last_len = len;
                    last_dist_m1 = dist_m1;
                    while (len--) {
                        u->window[u->curr_pos] =
                            u->window[(u->curr_pos - dist) & (u->dic_size - 1)];
                        if (++u->curr_pos >= u->dic_size) {
                            write_dictionary(u, &write_pos, &to_flush);
                            u->curr_pos &= u->dic_size - 1;
                        }
                    }
                    cur_dist = dist;
                    prev_byte = u->window[(u->curr_pos - dist) & (u->dic_size - 1)];
                    n = (int)last_len;
                    new_state = 2;
                }
            } else {                    /* new match with fresh distance */
                uint32_t g2, base, extra = 0;
                int e;
                uint32_t len, dclass, d, eb, dm1, dist;
                e = fm_decode(u, &u->len_new[state], &b);
                if (e < 0)
                    return 0;
                g2 = (uint32_t)e;
                if (g2 > 8)
                    return 0;
                base = LT_BASE[g2];
                if (LT_NEXT[g2]) {
                    e = fm_decode(u, &u->sec_n[state][LT_NEXT[g2]], &b);
                    if (e < 0)
                        return 0;
                    extra = (uint32_t)e;
                }
                len = base + extra;
                if (len == 257) {
                    e = rc_raw(u, &b, 14);
                    if (e < 0)
                        return 0;
                    len = 257u + (uint32_t)e;
                }
                if (len == 2)
                    dclass = 0;
                else if (len == 3)
                    dclass = 1;
                else if (len == 4)
                    dclass = 2;
                else
                    dclass = 3;
                e = fm_decode(u, &u->len_dist[dclass], &b);
                if (e < 0)
                    return 0;
                d = (uint32_t)e;
                if (d > 49)
                    return 0;
                eb = DT_EBITS[d];
                dm1 = DT_BASE[d];
                if (eb > 0) {
                    if (eb >= 4) {
                        e = rc_raw(u, &b, eb - 4);
                        if (e < 0)
                            return 0;
                        dm1 += (uint32_t)e << 4;
                        e = fm_decode(u, &u->dist_low, &b);
                        if (e < 0)
                            return 0;
                        dm1 += (uint32_t)e;
                    } else {
                        e = rc_raw(u, &b, eb);
                        if (e < 0)
                            return 0;
                        dm1 += (uint32_t)e;
                    }
                }
                dist = dm1 + 1;
                if (dist == 0 || dist > u->dic_size)
                    return 0;
                if (len == 0 || len > remaining)
                    return 0;
                {
                    uint32_t l = len;
                    for (i = 3; i > 0; i--)
                        dist_mru[i] = dist_mru[i - 1];
                    dist_mru[0] = dm1;
                    while (l--) {
                        u->window[u->curr_pos] =
                            u->window[(u->curr_pos - dist) & (u->dic_size - 1)];
                        if (++u->curr_pos >= u->dic_size) {
                            write_dictionary(u, &write_pos, &to_flush);
                            u->curr_pos &= u->dic_size - 1;
                        }
                    }
                }
                cur_dist = dist;
                prev_byte = u->window[(u->curr_pos - dist) & (u->dic_size - 1)];
                /* NB: the original (sub_40D8B1) updates 2776/2780 only
                 * in the repeat branch, so last_len/last_dist_m1 are
                 * intentionally left untouched here. */
                n = (int)len;
                new_state = 1;
            }
        } else {
            ultra_lit *lit = state ? &u->lit[1][prev_byte] : &u->lit[0][prev_byte];
            int sym = lit_decode(u, lit, &b);
            if (sym < 0)
                return 0;
            u->window[u->curr_pos] = (uint8_t)sym;
            if (++u->curr_pos >= u->dic_size) {
                write_dictionary(u, &write_pos, &to_flush);
                u->curr_pos &= u->dic_size - 1;
            }
            prev_byte = (uint32_t)sym;
            n = 1;
            new_state = 0;
        }
        prev_state = state;
        state = (uint32_t)new_state;
        decoded += (uint64_t)n;
        remaining -= (uint64_t)n;
    }

    write_dictionary(u, &write_pos, &to_flush);
    return 1;
}
