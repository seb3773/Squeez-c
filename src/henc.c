/* henc.c — SQX Huffman encoder implementation (see henc.h).
 *
 * Codes are assigned identically to GenerateUCTab (ctab.c): cumulative
 * base addresses per length, symbols processed in ascending order.
 * The emitted codeword is `next[len] >> (mb - len)` on `len` bits, MSB first.
 */
#include "henc.h"
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- Codes */

int sqx_make_codes(const uint8_t *lens, uint32_t n, uint32_t mb, uint16_t *codes)
{
    uint32_t len_count[18] = {0};
    uint32_t next[19] = {0};

    if (mb + 2 > 18)
        return 0;
    for (uint32_t i = 0; i < n; i++) {
        if (lens[i] > mb)
            return 0;
        len_count[lens[i]]++;
    }

    next[1] = 0;
    for (uint32_t c = 1; c <= mb; c++)
        next[c + 1] = next[c] + (len_count[c] << (mb - c));

    if (next[mb + 1] != (1u << mb))
        return 0;                       /* Incomplete tree: unreadable */

    for (uint32_t sym = 0; sym < n; sym++) {
        uint32_t l = lens[sym];
        if (l == 0)
            continue;
        codes[sym] = (uint16_t)(next[l] >> (mb - l));
        next[l] += 1u << (mb - l);
    }
    return 1;
}

/* ---------------------------------------- Code lengths from frequencies */

/* Standard Huffman over `n` symbols with frequencies `freq` (0 = absent).
 * `depth[sym]` receives the tree depth (0 if absent). Returns maximum depth.
 * Sorted active list (frequency, index): exact and O(n^2), n <= 311. */
static uint32_t huff_depths(const uint32_t *freq, uint32_t n, uint32_t *depth)
{
    typedef struct { uint64_t f; int idx; } hnode;
    hnode *act;
    uint32_t *parent;
    uint32_t nact = 0, next_node = n, maxdepth = 0;

    act = (hnode *)malloc(sizeof(hnode) * (2 * n + 1));
    parent = (uint32_t *)malloc(sizeof(uint32_t) * (2 * n + 1));
    if (!act || !parent) { free(act); free(parent); return 0; }
    memset(parent, 0xFF, sizeof(uint32_t) * (2 * n + 1));

    for (uint32_t i = 0; i < n; i++)
        if (freq[i] > 0) {
            act[nact].f = freq[i];
            act[nact].idx = (int)i;
            nact++;
        }
    if (nact == 0) { free(act); free(parent); return 0; }

    while (nact > 1) {
        hnode a = act[0], b = act[1];
        hnode c;
        uint32_t i;

        parent[a.idx] = next_node;
        parent[b.idx] = next_node;
        c.f = a.f + b.f;
        c.idx = (int)next_node++;
        memmove(act, act + 2, sizeof(hnode) * (nact - 2));
        nact -= 2;
        for (i = 0; i < nact; i++)
            if (act[i].f > c.f || (act[i].f == c.f && act[i].idx > c.idx))
                break;
        memmove(act + i + 1, act + i, sizeof(hnode) * (nact - i));
        act[i] = c;
        nact++;
    }

    for (uint32_t i = 0; i < n; i++) {
        uint32_t d = 0, node = i;
        if (freq[i] == 0) { depth[i] = 0; continue; }
        while (parent[node] != 0xFFFFFFFFu) {
            node = parent[node];
            if (++d > 64) break;
        }
        depth[i] = d;
        if (d > maxdepth) maxdepth = d;
    }
    free(act);
    free(parent);
    return maxdepth;
}

/* Adjust lengths to satisfy `limit` and Kraft inequality:
 * progressively flatten distribution (right-shift frequencies) until depths fit. */
int sqx_lens_from_freq(const uint32_t *freq, uint32_t n, uint32_t limit,
                       uint8_t *lens)
{
    uint32_t used = 0, last_used = 0;
    uint32_t *tmp, *depth;
    uint32_t maxdepth = 0;

    memset(lens, 0, n);
    if (limit == 0 || limit > 15)
        return 0;
    for (uint32_t i = 0; i < n; i++)
        if (freq[i] > 0) { used++; last_used = i; }
    if (used == 0)
        return 0;

    tmp = (uint32_t *)malloc(sizeof(uint32_t) * n);
    depth = (uint32_t *)malloc(sizeof(uint32_t) * n);
    if (!tmp || !depth) { free(tmp); free(depth); return 0; }

    for (uint32_t shift = 0; shift < 40; shift++) {
        uint64_t total = 0;
        for (uint32_t i = 0; i < n; i++) {
            tmp[i] = freq[i] ? (freq[i] >> shift) : 0;
            total += tmp[i];
        }
        /* Keep at least 1 count to avoid dropping present symbols */
        if (shift > 0 || total == 0)
            for (uint32_t i = 0; i < n; i++)
                if (freq[i] && !tmp[i])
                    tmp[i] = 1;

        /* Single symbol case: impossible to form a complete tree without a dummy symbol */
        if (used == 1) {
            for (uint32_t i = 0; i < n; i++)
                if (tmp[i] == 0) { tmp[i] = 1; break; }
        }

        maxdepth = huff_depths(tmp, n, depth);
        if (maxdepth <= limit)
            break;
    }
    if (used == 1 && maxdepth == 0)
        maxdepth = 1;

    if (maxdepth > limit) {
        free(tmp);
        free(depth);
        return 0;
    }
    for (uint32_t i = 0; i < n; i++)
        lens[i] = (uint8_t)depth[i];
    (void)last_used;
    free(tmp);
    free(depth);
    return 1;
}

/* --------------------------------------------------- Flat serialization */

/* Inverse of ReadOneTabFlat:
 * Tokens: 0 = same value, 10 = value+1, 111 + 4 bits = new value,
 * 1100 = value-1, 1101 = value+2. */
void sqx_write_flat_tab(sqx_bitw *w, const uint8_t *lens, uint32_t n)
{
    uint32_t count = 1, cur;

    SQX_PUT(w, lens[0], 4);
    cur = lens[0];
    while (count < n) {
        uint32_t v = lens[count];
        if (v == cur) {
            SQX_PUT(w, 0, 1);
        } else if (v == cur + 1) {
            SQX_PUT(w, 0x2, 2);           /* 10 */
            cur = v;
        } else if (v + 1 == cur) {
            SQX_PUT(w, 0xC, 4);           /* 1100 */
            cur = v;
        } else if (v == cur + 2) {
            SQX_PUT(w, 0xD, 4);           /* 1101 */
            cur = v;
        } else {
            SQX_PUT(w, 0x7, 3);           /* 111 */
            SQX_PUT(w, v, 4);
            cur = v;
        }
        count++;
    }
}

/* Count meta-symbols (0..18) emitted by tree serialization */
void sqx_meta_freq(const uint8_t *lens, uint32_t n, uint32_t *mfreq)
{
    uint32_t i = 0;

    while (i < n) {
        uint32_t v = lens[i], run = 1;
        while (i + run < n && lens[i + run] == v)
            run++;
        if (v == 0 && run >= 11) {
            mfreq[18]++;
            i += run > 138 ? 138 : run;
            continue;
        }
        if (v == 0 && run >= 3) {
            mfreq[17]++;
            i += run > 10 ? 10 : run;
            continue;
        }
        if (i > 0 && v == lens[i - 1] && run >= 3) {
            mfreq[16]++;
            i += run > 6 ? 6 : run;
            continue;
        }
        mfreq[v]++;
        i++;
    }
}

/* --------------------------------------------------- Tree serialization */

static void put_sym(sqx_bitw *w, uint32_t sym, const uint16_t *codes,
                    const uint8_t *lens)
{
    SQX_PUT(w, codes[sym], lens[sym]);
}

void sqx_write_tree_tab(sqx_bitw *w, const uint8_t *lens, uint32_t n,
                        const uint16_t *bitct_codes, const uint8_t *bitct_lens)
{
    uint32_t i = 0;

    while (i < n) {
        uint32_t v = lens[i], run = 1;
        while (i + run < n && lens[i + run] == v)
            run++;

        if (v == 0 && run >= 11) {
            uint32_t r = run > 138 ? 138 : run;
            put_sym(w, 18, bitct_codes, bitct_lens);
            SQX_PUT(w, r - 11, 7);
            i += r;
            continue;
        }
        if (v == 0 && run >= 3) {
            uint32_t r = run > 10 ? 10 : run;
            put_sym(w, 17, bitct_codes, bitct_lens);
            SQX_PUT(w, r - 3, 3);
            i += r;
            continue;
        }
        if (i > 0 && v == lens[i - 1] && run >= 3) {
            uint32_t r = run > 6 ? 6 : run;
            put_sym(w, 16, bitct_codes, bitct_lens);
            SQX_PUT(w, r - 3, 2);
            i += r;
            continue;
        }
        put_sym(w, v, bitct_codes, bitct_lens);
        i++;
    }
}
