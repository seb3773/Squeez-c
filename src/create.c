/* create.c — SQX encoder (LZ77 + Huffman): compresses files into .sqx archives
 * (v1.1 or v2.0, methods 1 to 4).
 *
 * The bit-layer (canonical codes, table serialization, block headers,
 * container) is identical to the one validated by `sqx repack` (bit-identical
 * to the oracle streams); only the parsing (match selection) is an encoder
 * heuristic here.
 */
#include "create.h"
#include "enc.h"
#include "henc.h"
#include "lz.h"
#include "ppm8.h"
#include "audio.h"
#include "sqx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define HASH3_BITS  16
#define HASH3_SIZE  (1u << HASH3_BITS)
#define HASH2_SIZE  65536u
#define MAX_MATCH   16640u          /* 257 + 16383: maximum SQX match length */
#define MIN_MATCH   3u
#define BLOCK_OPS   16383u          /* number of symbols per block (matching legacy oracle) */

static uint32_t hash3(const uint8_t *p)
{
    uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    return (v * 2654435761u) >> (32 - HASH3_BITS);
}

static uint32_t hash2(const uint8_t *p)
{
    return ((uint32_t)p[0] << 8) | p[1];
}

/* --------------------------------------------------------------- match finder */

typedef struct {
    const uint8_t *buf;
    uint32_t n, dic;
    int      max_chain;
    int32_t *prev, *head;
    int32_t  last2[HASH2_SIZE];
} matcher;

static int matcher_init(matcher *m, const uint8_t *buf, uint32_t n, uint32_t dic,
                        int max_chain)
{
    m->buf = buf;
    m->n = n;
    m->dic = dic;
    m->max_chain = max_chain;
    m->prev = (int32_t *)malloc(sizeof(int32_t) * ((size_t)n + 1));
    m->head = (int32_t *)malloc(sizeof(int32_t) * HASH3_SIZE);
    if (!m->prev || !m->head) {
        free(m->prev);
        free(m->head);
        return 0;
    }
    for (uint32_t i = 0; i < HASH3_SIZE; i++)
        m->head[i] = -1;
    for (uint32_t i = 0; i < HASH2_SIZE; i++)
        m->last2[i] = -1;
    for (uint32_t i = 0; i < n; i++)
        m->prev[i] = -1;
    return 1;
}

static void matcher_free(matcher *m)
{
    free(m->prev);
    free(m->head);
}

static void matcher_insert(matcher *m, uint32_t pos)
{
    if (pos + 2 >= m->n)
        return;
    m->prev[pos] = m->head[hash3(m->buf + pos)];
    m->head[hash3(m->buf + pos)] = (int32_t)pos;
    m->last2[hash2(m->buf + pos)] = (int32_t)pos;
}

static uint32_t match_len(const matcher *m, uint32_t pos, uint32_t dist,
                          uint32_t maxlen)
{
    uint32_t l = 0;
    const uint8_t *a = m->buf + pos, *b = m->buf + pos - dist;
    while (l < maxlen && a[l] == b[l])
        l++;
    return l;
}

/* Best standard match (>= MIN_MATCH); returns 1 if found */
static int best_match(matcher *m, uint32_t pos, uint32_t *out_len,
                      uint32_t *out_dist)
{
    uint32_t best_len = 0, best_dist = 0, maxlen = m->n - pos;
    int chain = m->max_chain;
    int32_t cand;

    if (maxlen > MAX_MATCH)
        maxlen = MAX_MATCH;
    if (maxlen < MIN_MATCH || pos + 2 >= m->n)
        return 0;
    cand = m->head[hash3(m->buf + pos)];
    while (cand >= 0 && chain-- > 0) {
        uint32_t dist = pos - (uint32_t)cand;
        if (dist == 0 || dist > m->dic)
            break;
        if (best_len < maxlen &&
            m->buf[(uint32_t)cand + best_len] == m->buf[pos + best_len]) {
            uint32_t l = match_len(m, pos, dist, maxlen);
            if (l > best_len) {
                best_len = l;
                best_dist = dist;
                if (l >= maxlen)
                    break;
            }
        }
        cand = m->prev[cand];
    }
    if (best_len >= MIN_MATCH) {
        *out_len = best_len;
        *out_dist = best_dist;
        return 1;
    }
    return 0;
}

/* Length 2 match (distance <= 255) via last occurrence of the pair */
static uint32_t match2(matcher *m, uint32_t pos, uint32_t *out_dist)
{
    int32_t c;
    if (pos + 1 >= m->n)
        return 0;
    c = m->last2[hash2(m->buf + pos)];
    if (c < 0)
        return 0;
    {
        uint32_t dist = pos - (uint32_t)c;
        if (dist == 0 || dist > SQX_MIN_LEN2)
            return 0;
        *out_dist = dist;
        return 2;
    }
}

/* ------------------------------------------------- symbol construction */

static uint32_t len2_sym(uint32_t dist, uint32_t *eb)
{
    uint32_t d = dist - 1;
    for (uint32_t k = 0; k < SQX_LEN2_CODES; k++)
        if (d >= sqx_len2_offsets[k] && d - sqx_len2_offsets[k] < (1u << sqx_len2_ebits[k])) {
            *eb = d - sqx_len2_offsets[k];
            return k;
        }
    return 0xFFFFFFFFu;
}

/* Length 3: in 2.0 the table has a 16th code (base 16384, 14 bits). */
static uint32_t len3_sym(uint32_t dist, uint32_t *eb, int fmt20)
{
    const uint32_t *ofs = fmt20 ? sqx_len3_offsets2 : sqx_len3_offsets;
    const uint32_t *ebs = fmt20 ? sqx_len3_ebits2 : sqx_len3_ebits;
    uint32_t n = fmt20 ? SQX_LEN3_CODES2 : SQX_LEN3_CODES;
    uint32_t d = dist - 1;

    for (uint32_t k = 0; k < n; k++)
        if (d >= ofs[k] && d - ofs[k] < (1u << ebs[k])) {
            *eb = d - ofs[k];
            return k;
        }
    return 0xFFFFFFFFu;
}

/* Length >= 4: code index + extra bits; 0xFFFFFFFF if impossible.
 * In 2.0 the decoder does NOT add distance correction
 * (`if (dist > MIN_LEN4) len++` only exists in 1.1). */
static uint32_t len_sym(uint32_t len, uint32_t dist, uint32_t *eb, int fmt20)
{
    uint32_t L = len - 4;

    if (!fmt20 && dist > SQX_MIN_LEN4 && len > 4)
        L = len - 5;
    if (L > 255)
        return 0xFFFFFFFFu;
    for (uint32_t k = 0; k < SQX_LEN_CODES - 1; k++) {
        uint32_t room = (k + 1 < SQX_LEN_CODES - 1)
                            ? (sqx_len_offsets[k + 1] - sqx_len_offsets[k])
                            : (256 - sqx_len_offsets[k]);
        if (L >= sqx_len_offsets[k] && L - sqx_len_offsets[k] < room) {
            *eb = L - sqx_len_offsets[k];
            return k;
        }
    }
    return 0xFFFFFFFFu;
}

/* Repeated offset length: (rep table symbol, extra bits) */
static uint32_t rep_len_sym(uint32_t len, uint32_t dist, uint32_t *eb,
                            uint32_t *l14, int fmt20)
{
    uint32_t L = len - 2;

    if (fmt20) {
        /* 2.0: single correction, using extended length 3 threshold (0x7FFF). */
        if (dist > 0x7FFFu)
            L--;
    } else {
        if (dist > SQX_MIN_LEN3)
            L--;
        if (dist > SQX_MIN_LEN4)
            L--;
    }
    if (L > 255) {
        /* Long form: decoder computes last_len = 257 + l14, WITHOUT
         * applying the distance corrections above. */
        if (len < 257 || len - 257 > 16383)
            return 0xFFFFFFFFu;
        *l14 = len - 257;
        return SQX_LEN_CODES - 1;
    }
    for (uint32_t k = 0; k < SQX_LEN_CODES - 1; k++)
        if (L >= sqx_len_offsets[k] && L - sqx_len_offsets[k] < (1u << sqx_len_ebits[k])) {
            *eb = L - sqx_len_offsets[k];
            return k;
        }
    return 0xFFFFFFFFu;
}

static uint32_t dist_sym(const sqx_dist_tab *dt, uint32_t dist, uint32_t *eb)
{
    uint32_t d = dist - 1;
    for (uint32_t c = 0; c < dt->num_codes; c++) {
        uint32_t base = dt->base_table[c], bits = dt->base_bits[c];
        if (d >= base && d - base < (1u << bits)) {
            *eb = d - base;
            return c;
        }
    }
    return 0xFFFFFFFFu;
}

/* Litlen symbol: 1.1 space -> format space */
static uint32_t to_raw(uint32_t s, int fmt20)
{
    if (!fmt20)
        return s;
    if (s <= 256)
        return s;
    return s >= 284 ? s + 2 : s + 1;
}

/* ------------------------------------------------------------ parse + IR */

typedef struct {
    sqx_op  *ops;
    size_t   n, cap;
    int      fmt20;
    uint32_t f_nc[SQX_NC2];
    uint32_t f_rep[SQX_LEN_CODES];
    uint32_t f_dist[SQX_DIST_CODES];
} parse_out;

/* Format symbol -> 1.1 space (to determine operator kind) */
static uint32_t from_raw(uint32_t s, int fmt20)
{
    if (!fmt20 || s <= 256)
        return s;
    return s >= 286 ? s - 2 : s - 1;
}

static void push_op(parse_out *p, const sqx_op *op, uint32_t adv)
{
    uint32_t v = from_raw(op->code, p->fmt20);

    ((sqx_op *)op)->adv = (uint16_t)adv;

    if (op->code < SQX_NC2)
        p->f_nc[op->code]++;
    if (v == 256) {
        /* nothing more */
    } else if (v >= 257 && v <= 260) {
        if (op->b < SQX_LEN_CODES)
            p->f_rep[op->b]++;
    } else if (v >= SQX_LEN_START) {
        if (op->b < SQX_DIST_CODES)
            p->f_dist[op->b]++;
    }

    if (p->n == p->cap) {
        size_t cap = p->cap ? p->cap * 2 : 4096;
        sqx_op *q = (sqx_op *)realloc(p->ops, sizeof(*q) * cap);
        if (!q)
            return;
        p->ops = q;
        p->cap = cap;
    }
    p->ops[p->n++] = *op;
}

static int lz_parse(const uint8_t *buf, size_t len, uint32_t dic, int effort,
                    int fmt20, parse_out *p)
{
    matcher m;
    uint32_t pos = 0;
    uint32_t last_len = 0, last_dist = 0, prev_dists[4] = {0, 0, 0, 0}, pi = 0;
    sqx_dist_tab dt;
    int lazy = effort >= 2;

    memset(p, 0, sizeof(*p));
    p->fmt20 = fmt20;
    sqx_set_base_dist_tabs(&dt, dic);
    if (!matcher_init(&m, buf, (uint32_t)len, dic, effort * 32))
        return 0;

    while (pos < (uint32_t)len) {
        sqx_op op;
        uint32_t blen = 0, bdist = 0, mlen2 = 0, mdist2 = 0;
        uint32_t maxlen = (uint32_t)len - pos;
        int use_rep = 0;
        uint32_t rep_code = 0, rep_dist = 0, rep_len = 0;
        uint32_t adv = 1;

        memset(&op, 0, sizeof(op));
        if (maxlen > MAX_MATCH)
            maxlen = MAX_MATCH;

        /* 1) Exact repetition of last (len, dist): free */
        if (last_dist && last_dist <= pos && last_len <= maxlen &&
            match_len(&m, pos, last_dist, last_len) >= last_len) {
            rep_code = 256;
            rep_dist = last_dist;
            rep_len = last_len;
            use_rep = 1;
        }
        /* 2) Repeated offsets (257..260) */
        if (!use_rep) {
            for (uint32_t k = 1; k <= 4; k++) {
                uint32_t d = prev_dists[(pi - k) & 3];
                uint32_t l;
                if (!d || d > pos)
                    continue;
                l = match_len(&m, pos, d, maxlen);
                if (l >= 2 && (!use_rep || l > rep_len)) {
                    use_rep = 1;
                    rep_code = 256 + k;
                    rep_dist = d;
                    rep_len = l;
                }
            }
        }
        /* 3) Standard match */
        if (best_match(&m, pos, &blen, &bdist)) {
            /* ok */
        } else if (match2(&m, pos, &mdist2)) {
            mlen2 = 2;
        }

        /* Choice: repeated form is cheaper, preferred for equal length
         * (>= standard match); length 2 match never overrides a repetition. */
        if (use_rep && (!blen || rep_len >= blen)) {
            uint32_t eb = 0, l14 = 0;
            uint32_t rs = rep_len_sym(rep_len, rep_dist, &eb, &l14, fmt20);
            if (rs == 0xFFFFFFFFu) {
                use_rep = 0;
            } else {
                op.code = (uint16_t)to_raw(rep_code, fmt20);
                op.b = (uint16_t)rs;
                if (rs == SQX_LEN_CODES - 1)
                    op.l14 = (uint16_t)l14;
                else
                    op.c = eb;
                adv = rep_len;
                last_len = rep_len;
                last_dist = rep_dist;
                prev_dists[pi++ & 3] = rep_dist;
                push_op(p, &op, adv);
                for (uint32_t i = 1; i < rep_len; i++)
                    matcher_insert(&m, pos + i);
                pos += rep_len;
                continue;
            }
        }

        /* Search finished: current position enters the hash chain
         * (it must not be a candidate for itself) */
        matcher_insert(&m, pos);

        /* Lazy matching: is next match better? */
        if (lazy && blen >= MIN_MATCH && blen < MAX_MATCH && pos + 1 < (uint32_t)len) {
            uint32_t nl = 0, nd = 0;
            if (best_match(&m, pos + 1, &nl, &nd) && nl > blen && nl > 3)
                blen = 0;                          /* emit literal */
        }

        if (blen >= MIN_MATCH) {
            if (blen == 3) {
                /* Note: the 16th length 3 code in 2.0 (symbol 285, distance
                 * > 0x3FFF) has no equivalent in 1.1 symbol space used
                 * internally by parse: do not emit. */
                uint32_t eb = 0, k = len3_sym(bdist, &eb, fmt20);
                if (k != 0xFFFFFFFFu && k < SQX_LEN3_CODES &&
                    bdist <= SQX_MIN_LEN3) {
                    op.code = (uint16_t)to_raw(SQX_LEN3_START + k, fmt20);
                    op.a = (uint16_t)eb;
                } else {
                    blen = 0;
                }
            } else {
                uint32_t eb = 0, k = len_sym(blen, bdist, &eb, fmt20);
                uint32_t de = 0, ds;
                if (!fmt20 && blen == 4 && bdist > SQX_MIN_LEN4)
                    /* In 1.1 decoder adds 1 to lengths >= 4 beyond
                     * MIN_LEN4: an exact length 4 match is not
                     * representable, emit literal. */
                    blen = 0;
                else if (k == 0xFFFFFFFFu) {
                    blen = 0;
                } else if (blen >= 258) {          /* 14-bit long form */
                    op.code = (uint16_t)to_raw(SQX_NC - 2, fmt20);
                    op.l14 = (uint16_t)(blen - 257);
                    ds = dist_sym(&dt, bdist, &de);
                    if (ds == 0xFFFFFFFFu) {
                        blen = 0;
                    } else {
                        op.b = (uint16_t)ds;
                        op.c = de;
                    }
                } else {
                    ds = dist_sym(&dt, bdist, &de);
                    if (ds == 0xFFFFFFFFu) {
                        blen = 0;
                    } else {
                        op.code = (uint16_t)to_raw(SQX_LEN_START + k, fmt20);
                        op.a = (uint16_t)eb;
                        op.b = (uint16_t)ds;
                        op.c = de;
                    }
                }
            }
            if (blen) {
                /* Note: SDK only updates uLastLen/uLastDist in repeated offset
                 * forms (cf. CopyString): standard match does not feed code 256. */
                adv = blen;
                prev_dists[pi++ & 3] = bdist;
                push_op(p, &op, adv);
                for (uint32_t i = 1; i < blen; i++)
                    matcher_insert(&m, pos + i);
                pos += blen;
                continue;
            }
        }

        if (mlen2 == 2) {
            uint32_t eb = 0, k = len2_sym(mdist2, &eb);
            if (k != 0xFFFFFFFFu) {
                adv = 2;
                op.code = (uint16_t)to_raw(SQX_LEN2_START + k, fmt20);
                op.a = (uint16_t)eb;
                prev_dists[pi++ & 3] = mdist2;
                push_op(p, &op, adv);
                matcher_insert(&m, pos + 1);
                pos += 2;
                continue;
            }
        }

        /* literal */
        op.code = (uint16_t)to_raw(buf[pos], fmt20);
        push_op(p, &op, adv);
        pos++;
    }

    matcher_free(&m);
    return 1;
}

/* -------------------------------------------------------------- tables */

/* Diagnostic: operator length as computed by the DECODER.
 * Used to verify that parsing emits exactly `orig_size` bytes. */
static uint32_t op_decoded_len(const parse_out *p, const sqx_op *op,
                               uint32_t last_len, uint32_t last_dist,
                               const sqx_dist_tab *dt, int *bad)
{
    uint32_t v = from_raw(op->code, p->fmt20);

    *bad = 0;
    if (v < 256)
        return 1;
    if (v == 256)
        return last_len;
    if (v >= 257 && v <= 260) {
        if (op->b == SQX_LEN_CODES - 1)
            return 257 + op->l14;         /* long form: no correction */
        return sqx_len_offsets[op->b] + (sqx_len_ebits[op->b] ? op->c : 0) + 2 +
               (p->fmt20 ? (last_dist > 0x7FFFu)
                         : ((last_dist > SQX_MIN_LEN3) +
                            (last_dist > SQX_MIN_LEN4)));
    }
    if (v >= 261 && v <= 268)
        return 2;
    if (v >= 269 && v <= 283)
        return 3;
    if (v == SQX_NC - 2) {
        uint32_t len = 257 + op->l14;
        uint32_t d = dt->base_table[op->b] + (dt->base_bits[op->b] ? op->c : 0) + 1;
        if (!p->fmt20 && d > SQX_MIN_LEN4)
            *bad = 1;                        /* long form does not add 1 */
        return len;
    }
    if (v >= SQX_LEN_START && v < SQX_NC - 2) {
        uint32_t k = v - SQX_LEN_START;
        uint32_t len = sqx_len_offsets[k] + (sqx_len_ebits[k] ? op->a : 0) + 4;
        uint32_t d = dt->base_table[op->b] + (dt->base_bits[op->b] ? op->c : 0) + 1;
        if (!p->fmt20 && d > SQX_MIN_LEN4)
            len++;
        return len;
    }
    *bad = 1;
    return 0;
}

/* Replays operator list like the decoder does (without bits) and
 * verifies that produced bytes match the original. */
static void verify_ops(const parse_out *p, size_t orig_size, uint32_t dic)
{
    sqx_dist_tab dt;
    uint32_t last_len = 0, last_dist = 0, pd[4] = {0, 0, 0, 0}, pi = 0;
    uint64_t produced = 0;
    long first_bad = -1;

    sqx_set_base_dist_tabs(&dt, dic);
    for (size_t i = 0; i < p->n; i++) {
        const sqx_op *op = &p->ops[i];
        uint32_t v = from_raw(op->code, p->fmt20);
        uint32_t d, l;
        int bad = 0;

        l = op_decoded_len(p, op, last_len, last_dist, &dt, &bad);
        if (v < 256) {
            /* literal: state unchanged */
        } else if (v == 256) {
            /* nothing changes */
        } else if (v >= 257 && v <= 260) {
            last_dist = pd[(pi - (v - 256)) & 3];
            l = op_decoded_len(p, op, last_len, last_dist, &dt, &bad);
            pd[pi++ & 3] = last_dist;
            last_len = l;
        } else if (v >= 261 && v <= 283) {
            /* length 2 or 3: PrevDists only (not uLastLen/uLastDist) */
            d = (v <= 268 ? sqx_len2_offsets[v - SQX_LEN2_START] +
                                (sqx_len2_ebits[v - SQX_LEN2_START] ? op->a : 0)
                          : sqx_len3_offsets[v - SQX_LEN3_START] +
                                (sqx_len3_ebits[v - SQX_LEN3_START] ? op->a : 0)) + 1;
            pd[pi++ & 3] = d;
        } else {
            if (op->b < dt.num_codes) {
                d = dt.base_table[op->b] +
                    (dt.base_bits[op->b] ? op->c : 0) + 1;
                pd[pi++ & 3] = d;
            }
        }
        if (bad)
            fprintf(stderr, "[create] op %zu: code=%u ambiguous form\n", i,
                    op->code);
        if (getenv("SQX_CREATE_TRACE3"))
            fprintf(stderr, "E %zu %u %u %u %u %u %u\n", i, op->code,
                    op->a, op->b, op->c, op->l14, op->adv);
        if (getenv("SQX_CREATE_TRACE2") && i >= 20 && i < 34)
            fprintf(stderr, "  op %zu: code=%u adv=%u dec=%u last_len=%u "
                            "last_dist=%u pi=%u\n",
                    i, op->code, op->adv, l, last_len, last_dist, pi);
        if (l != op->adv && first_bad < 0) {
            first_bad = (long)i;
            fprintf(stderr, "[create] op %zu: encoder=%u bytes, decoder=%u "
                            "(code=%u b=%u a=%u c=%u l14=%u)\n",
                    i, op->adv, l, op->code, op->b, op->a, op->c, op->l14);
        }
        produced += l;
        if (produced > orig_size) {
            fprintf(stderr, "[create] op %zu: overflow (%llu > %zu)\n", i,
                    (unsigned long long)produced, orig_size);
            return;
        }
    }
    if (produced != orig_size)
        fprintf(stderr, "[create] parse: %llu bytes produced, %zu expected "
                        "(diff %lld), 1st discrepancy at op %ld\n",
                (unsigned long long)produced, orig_size,
                (long long)((int64_t)orig_size - (int64_t)produced), first_bad);
}

#define TRACE_TABLES(what)                                                    \
    do {                                                                      \
        if (getenv("SQX_CREATE_TRACE"))                                       \
            fprintf(stderr, "[create] table %s failed\n", (what));        \
    } while (0)

/* Table lengths + bitct from symbol frequencies */
static int build_tables(const parse_out *p, int fmt20,
                        uint8_t *lit_lens, uint8_t *rep_lens, uint8_t *dist_lens,
                        uint8_t *bitct_lens)
{
    uint32_t mfreq[SQX_BCODES];
    uint32_t nc = fmt20 ? SQX_NC2 : SQX_NC;
    uint32_t nd = fmt20 ? SQX_DIST_CODES2 : SQX_DIST_CODES;

    /* An empty table (no symbol used) must remain transmissible:
     * two symbols of length 1 form a complete tree of 2 leaves. */
    if (!sqx_lens_from_freq(p->f_nc, nc, SQX_LCODE_BITS, lit_lens)) {
        TRACE_TABLES("litlen");
        return 0;                             /* impossible: whole stream */
    }
    if (!sqx_lens_from_freq(p->f_rep, SQX_LEN_CODES, SQX_LCODE_BITS, rep_lens)) {
        memset(rep_lens, 0, SQX_LEN_CODES);
        rep_lens[0] = 1;
        rep_lens[1] = 1;
        TRACE_TABLES("rep (empty)");
    }
    if (!sqx_lens_from_freq(p->f_dist, nd, SQX_LCODE_BITS, dist_lens)) {
        memset(dist_lens, 0, nd);
        dist_lens[0] = 1;
        dist_lens[1] = 1;
        TRACE_TABLES("dist (empty)");
    }

    memset(mfreq, 0, sizeof(mfreq));
    sqx_meta_freq(lit_lens, nc, mfreq);
    sqx_meta_freq(rep_lens, SQX_LEN_CODES, mfreq);
    sqx_meta_freq(dist_lens, nd, mfreq);
    if (!sqx_lens_from_freq(mfreq, SQX_BCODES, SQX_BCODE_BITS, bitct_lens)) {
        TRACE_TABLES("bitct");
        return 0;
    }
    return 1;
}

/* -------------------------------------------------------------- stream */

int sqx_create_stream(const uint8_t *buf, size_t len, int arcer_ver,
                      uint32_t dic_size, int effort, uint8_t **out,
                      size_t *out_len)
{
    parse_out p;
    sqx_ir ir;
    sqx_bitw w;
    uint8_t lit_lens[SQX_NC2], rep_lens[SQX_LEN_CODES];
    uint8_t dist_lens[SQX_DIST_CODES], bitct_lens[SQX_BCODES];
    int fmt20 = arcer_ver >= SQX_ARCVER_20;

    *out = NULL;
    *out_len = 0;
    if (!lz_parse(buf, len, dic_size, effort, fmt20, &p)) {
        if (getenv("SQX_CREATE_TRACE"))
            fprintf(stderr, "[create] parsing failed (%zu bytes)\n", len);
        return 0;
    }
    if (getenv("SQX_CREATE_TRACE"))
        verify_ops(&p, len, dic_size);
    if (!build_tables(&p, fmt20, lit_lens, rep_lens, dist_lens, bitct_lens)) {
        if (getenv("SQX_CREATE_TRACE"))
            fprintf(stderr, "[create] table generation failed (%zu ops)\n", p.n);
        free(p.ops);
        return 0;
    }

    sqx_ir_init(&ir, arcer_ver, 0, dic_size, len);
    {
        sqx_ir_block *b = NULL;
        for (size_t i = 0; i < p.n; i++) {
            if (!b || b->op_count == BLOCK_OPS) {
                b = sqx_ir_new_block(&ir);
                if (!b) { sqx_ir_free(&ir); free(p.ops); return 0; }
                b->kind = 0;
                b->blk_flag = 0;
                b->four_trees = 0;
                b->code_type = 0;              /* "tree" tables */
                b->max_tree_code = 0;
                b->num_litlen = fmt20 ? SQX_NC2 : SQX_NC;
                b->num_rep = SQX_LEN_CODES;
                b->num_dist = fmt20 ? SQX_DIST_CODES2 : SQX_DIST_CODES;
                memcpy(b->bitct_lens, bitct_lens, SQX_BCODES);
                memcpy(b->litlen_lens[0], lit_lens, b->num_litlen);
                memcpy(b->litlen_lens[1], lit_lens, b->num_litlen);
                memcpy(b->rep_lens, rep_lens, SQX_LEN_CODES);
                memcpy(b->dist_lens, dist_lens, SQX_DIST_CODES);
            }
            sqx_ir_add_op(&ir, &p.ops[i]);
        }
        for (size_t i = 0; i < ir.num_blocks; i++)
            ir.blocks[i].bt = (uint32_t)ir.blocks[i].op_count;
    }

    sqx_bitw_init(&w, (size_t)len + 1024);
    if (!sqx_enc_ir(&ir, NULL, 0, &w)) {
        if (getenv("SQX_CREATE_TRACE"))
            fprintf(stderr, "[create] encoding failed (%zu blocks)\n",
                    ir.num_blocks);
        sqx_bitw_free(&w);
        sqx_ir_free(&ir);
        free(p.ops);
        return 0;
    }
    *out = w.buf;
    *out_len = w.len;
    if (ir.num_blocks == 0) {
        sqx_bitw_free(&w);
        *out = NULL;
        *out_len = 0;
    }
    sqx_ir_free(&ir);
    free(p.ops);
    return 1;
}

/* -------------------------------------------------------------- container */

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Bits 8-10 of file header flags = dictionary size */
static uint16_t dict_flags(uint32_t dic_size, int arcer_ver)
{
    uint32_t n = 0;

    if (arcer_ver >= SQX_ARCVER_20 && dic_size > 4096u * 1024u) {
        while (n < 7 && ((8192u << n) * 1024u) < dic_size)
            n++;
        return (uint16_t)(0x0800u | (n << 8));       /* extended dictionary */
    }
    while (n < 7 && (32768u << n) < dic_size)
        n++;
    return (uint16_t)(n << 8);
}

static uint32_t dos_time(time_t t)
{
    struct tm tm, *lt;
    uint32_t d, h;
    if (t == 0)
        return 0;
    lt = localtime(&t);
    if (!lt)
        return 0;
    tm = *lt;
    d = (uint32_t)(tm.tm_mday | ((tm.tm_mon + 1) << 5) |
                   (((tm.tm_year + 1900 - 1980) & 0x7F) << 9));
    h = (uint32_t)((tm.tm_sec / 2) | (tm.tm_min << 5) | (tm.tm_hour << 11));
    return (d << 16) | h;
}

static void block_hdr(uint8_t *dst, uint8_t type, uint16_t flags, uint16_t size)
{
    put16(dst, 0);
    dst[2] = type;
    put16(dst + 3, flags);
    put16(dst + 5, size);
}

static void fix_crc(uint8_t *blk, size_t size)
{
    put16(blk, sqx_crc16(blk + 2, size - 2));
}

static int read_whole(const char *path, uint8_t **buf, size_t *len, uint32_t *attr)
{
    FILE *f = fopen(path, "rb");
    long n;
    struct stat st;
    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    n = ftell(f);
    rewind(f);
    *buf = (uint8_t *)malloc((size_t)n + 8);
    if (!*buf) { fclose(f); return 0; }
    if (n > 0 && fread(*buf, 1, (size_t)n, f) != (size_t)n) {
        free(*buf); fclose(f); return 0;
    }
    memset(*buf + n, 0, 8);
    *len = (size_t)n;
    fclose(f);
    *attr = 0x80;                                     /* archive attribute */
    if (stat(path, &st) == 0)
        *attr = (uint32_t)(st.st_mode & 0400 ? 0x20 : 0) | 0x80;
    return 1;
}

int sqx_create_archive(const char *out_path, char *const *in_paths, int n_in,
                       int arcer_ver, int method, uint32_t dic_size,
                       unsigned ppm_order, unsigned ppm_mem_mb)
{
    FILE *out;
    uint8_t *main_blk;
    sqx_ppm8 *p8 = NULL;
    sqx_audio audio;
    uint32_t audio_crc = 0;
    int have_audio_crc = 0;
    int rc = 0;

    if (n_in < 1)
        return 1;
    if (method == SQX_METHOD_TEXT) {
        if (arcer_ver < SQX_ARCVER_20) {
            fprintf(stderr, "Method 6 (PPMd) is only supported in SQX 2.0 format\n");
            return 1;
        }
        p8 = sqx_ppm8_new();
        if (!p8)
            return 1;
    }
    if (method == SQX_METHOD_AUDIO) {
        if (arcer_ver < SQX_ARCVER_20) {
            fprintf(stderr, "Audio encoding is only supported in SQX 2.0 format\n");
            return 1;
        }
        if (!sqx_audio_alloc(&audio)) {
            sqx_ppm8_free(p8);
            return 1;
        }
    }
    out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "%s: cannot create file\n", out_path);
        sqx_ppm8_free(p8);
        if (method == SQX_METHOD_AUDIO)
            sqx_audio_free(&audio);
        return 1;
    }

    /* MAIN block (25 bytes) */
    main_blk = (uint8_t *)calloc(1, 25);
    block_hdr(main_blk, SQX_BLOCK_MAIN, 0x0004, 25);
    memcpy(main_blk + 7, "-sqx-", 5);
    main_blk[12] = 0x0b;                              /* container version */
    fix_crc(main_blk, 25);
    if (fwrite(main_blk, 1, 25, out) != 25)
        rc = 1;
    free(main_blk);

    for (int i = 0; i < n_in && !rc; i++) {
        const char *path = in_paths[i];
        const char *base = strrchr(path, '/');
        uint8_t *buf = NULL, *data = NULL;
        size_t len = 0, dlen = 0, nl, hsz;
        uint32_t attr = 0x80, crc, dtm = 0;
        struct stat st;
        uint8_t *hdr;

        base = base ? base + 1 : path;
        if (!read_whole(path, &buf, &len, &attr)) {
            fprintf(stderr, "%s: cannot read file\n", path);
            rc = 1;
            break;
        }
        if (method == SQX_METHOD_TEXT) {
            /* PPMd (method 6): model matches decoder; each file carries
             * its own model (archive is not marked solid), so the stream
             * begins with its full header. */
            if (!sqx_ppm8_encode(p8, buf, len, ppm_order, ppm_mem_mb, 1,
                                 &data, &dlen)) {
                fprintf(stderr, "%s: PPMd compression failed\n", path);
                free(buf);
                rc = 1;
                break;
            }
        } else if (method == SQX_METHOD_AUDIO) {
            /* Audio 2.0 16-bit: container stores "audio" CRC
             * (18-byte header + file), cf. cx_audio.c. */
            uint32_t acrc = 0;

            if (!sqx_audio_encode(&audio, buf, len, &data, &dlen, &acrc)) {
                fprintf(stderr, "%s: audio compression failed"
                                " (16-bit PCM WAV expected)\n", path);
                free(buf);
                rc = 1;
                break;
            }
            audio_crc = acrc;
            have_audio_crc = 1;
        } else if (!sqx_create_stream(buf, len, arcer_ver, dic_size, method,
                                      &data, &dlen)) {
            fprintf(stderr, "%s: compression failed\n", path);
            free(buf);
            rc = 1;
            break;
        }
        if (stat(path, &st) == 0)
            dtm = dos_time(st.st_mtime);
        nl = strlen(base);
        hsz = 7 + 28 + nl;
        hdr = (uint8_t *)calloc(1, hsz);
        if (!hdr) { free(buf); free(data); rc = 1; break; }
        block_hdr(hdr, SQX_BLOCK_FILE, dict_flags(dic_size, arcer_ver),
                  (uint16_t)hsz);
        {
            uint8_t *pl = hdr + 7;
            crc = have_audio_crc ? audio_crc
                                 : sqx_crc32_raw(SQX_CRC32_INIT, buf, len);
            have_audio_crc = 0;
            pl[0] = 0;                                /* comp_flags */
            pl[1] = 0; pl[2] = 0;                     /* extra_flags */
            pl[3] = 0x0c;                             /* os/filesys (matching legacy sqc) */
            pl[4] = (uint8_t)(arcer_ver >= SQX_ARCVER_20 ? 0x14 : 0x0b);
            pl[5] = (uint8_t)method;
            put32(pl + 6, crc);
            put32(pl + 10, attr);
            put32(pl + 14, dtm);
            put32(pl + 18, (uint32_t)dlen);
            put32(pl + 22, (uint32_t)len);
            put16(pl + 26, (uint32_t)nl);
            memcpy(pl + 28, base, nl);
        }
        fix_crc(hdr, hsz);
        if (fwrite(hdr, 1, hsz, out) != hsz ||
            (dlen && fwrite(data, 1, dlen, out) != dlen))
            rc = 1;
        free(hdr);
        free(data);
        free(buf);
    }

    /* END block */
    if (!rc) {
        uint8_t end[7];
        block_hdr(end, SQX_BLOCK_END, 0, 7);
        fix_crc(end, 7);
        if (fwrite(end, 1, 7, out) != 7)
            rc = 1;
    }
    fclose(out);
    sqx_ppm8_free(p8);
    if (method == SQX_METHOD_AUDIO)
        sqx_audio_free(&audio);
    return rc;
}
