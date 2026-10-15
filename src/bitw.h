/* bitw.h — SQX bit writer, exact mirror of bitio.h / arcio.h.
 *
 * The bitstream is a sequence of 32-bit little-endian words; bits are packed
 * from MSB down to LSB within each word (the dual of NEEDBITS(n) = word >> (32-n)).
 * The final word is zero-padded (all observed LZ archives satisfy u32CompSize % 4 == 0).
 */
#ifndef SQX_BITW_H
#define SQX_BITW_H

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    size_t   len;      /* Written bytes */
    size_t   cap;
    uint32_t word;     /* Current 32-bit word accumulator */
    unsigned nbits;    /* Valid bits placed in word (0..31) */
    int      err;      /* 1 = memory allocation failed */
} sqx_bitw;

static inline void sqx_bitw_init(sqx_bitw *w, size_t cap)
{
    w->buf = (uint8_t *)malloc(cap ? cap : 64);
    w->len = 0;
    w->cap = w->buf ? (cap ? cap : 64) : 0;
    w->word = 0;
    w->nbits = 0;
    w->err = w->buf ? 0 : 1;
}

static inline void sqx_bitw_free(sqx_bitw *w)
{
    free(w->buf);
    w->buf = NULL;
    w->len = w->cap = 0;
}

static inline void sqx_bitw_reserve(sqx_bitw *w, size_t extra)
{
    if (w->err)
        return;
    if (w->len + extra <= w->cap)
        return;
    {
        size_t cap = w->cap ? w->cap : 64;
        uint8_t *p;
        while (cap < w->len + extra)
            cap *= 2;
        p = (uint8_t *)realloc(w->buf, cap);
        if (!p) { w->err = 1; return; }
        w->buf = p;
        w->cap = cap;
    }
}

/* Write the n least-significant bits of val, from MSB to LSB */
#define SQX_PUT(w, val, n) sqx_bitw_put((w), (uint32_t)(val), (unsigned)(n))

static inline void sqx_bitw_word_out(sqx_bitw *w)
{
    sqx_bitw_reserve(w, 4);
    if (w->err)
        return;
    w->buf[w->len++] = (uint8_t)(w->word);
    w->buf[w->len++] = (uint8_t)(w->word >> 8);
    w->buf[w->len++] = (uint8_t)(w->word >> 16);
    w->buf[w->len++] = (uint8_t)(w->word >> 24);
    w->word = 0;
    w->nbits = 0;
}

static inline void sqx_bitw_put(sqx_bitw *w, uint32_t val, unsigned n)
{
    if (w->err || n == 0)
        return;
    if (n > 32)
        n = 32;
    if (n < 32)
        val &= (uint32_t)((1ull << n) - 1u);

    /* Bits can cross 32-bit word boundaries */
    while (n > 0) {
        unsigned room = 32 - w->nbits;
        unsigned take = n < room ? n : room;
        uint32_t part = (take == n) ? val : (val >> (n - take));

        w->word |= part << (room - take);
        w->nbits += take;
        n -= take;
        if (w->nbits == 32)
            sqx_bitw_word_out(w);
    }
}

/* Terminate stream: pad current word with zeros to 32-bit boundary */
static inline void sqx_bitw_flush(sqx_bitw *w)
{
    if (w->err || w->nbits == 0)
        return;
    sqx_bitw_word_out(w);
}

/* Number of bits written so far */
static inline uint64_t sqx_bitw_bits(const sqx_bitw *w)
{
    return (uint64_t)w->len * 8 + w->nbits;
}

#endif /* SQX_BITW_H */
