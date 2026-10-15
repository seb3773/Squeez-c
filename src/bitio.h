/* bitio.h — SQX bit reader, matching official SDK semantics (arcio.h).
 *
 * The stream is read as 32-bit little-endian words; bits are extracted
 * from MSB to LSB within each word (NEEDBITS(n) = word >> (32-n)).
 * THIS IS NOT standard byte-by-byte MSB-first order: exact compliance
 * with this word-based ordering is mandatory for bit-exact compatibility.
 */
#ifndef SQX_BITIO_H
#define SQX_BITIO_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *buf;
    size_t len;          /* Stream length in bytes */
    size_t wpos;         /* Current 32-bit word index */
    uint32_t word;       /* Current 32-bit word, aligned to current bit */
    unsigned bits;       /* Number of bits consumed in window (0..31) */
} sqx_bits;

static inline uint32_t sqx_bits_word(const sqx_bits *b, size_t w)
{
    size_t o = w * 4;
    if (o + 4 <= b->len)
        return (uint32_t)b->buf[o] | ((uint32_t)b->buf[o + 1] << 8) |
               ((uint32_t)b->buf[o + 2] << 16) | ((uint32_t)b->buf[o + 3] << 24);
    /* Beyond end of stream: pad with zeros (encoder pads final word with zeros) */
    {
        uint32_t v = 0;
        for (size_t i = 0; i < 4 && o + i < b->len; i++)
            v |= (uint32_t)b->buf[o + i] << (8 * i);
        return v;
    }
}

static inline void sqx_bits_init(sqx_bits *b, const uint8_t *buf, size_t len)
{
    b->buf = buf;
    b->len = len;
    b->wpos = 0;
    b->bits = 0;
    b->word = len ? sqx_bits_word(b, 0) : 0;
}

/* Peek n bits (1..32) without consuming them */
static inline uint32_t sqx_bits_peek(const sqx_bits *b, unsigned n)
{
    if (n == 0) return 0;
    return n >= 32 ? b->word : (b->word >> (32 - n));
}

/* Consume n bits (0..32) */
static inline void sqx_bits_dump(sqx_bits *b, unsigned n)
{
    b->bits += n;
    b->wpos += b->bits >> 5;
    b->bits &= 31;
    b->word = sqx_bits_word(b, b->wpos) << b->bits;
    if (b->bits)
        b->word |= sqx_bits_word(b, b->wpos + 1) >> (32 - b->bits);
}

/* Direct read of n bits (1..32) */
static inline uint32_t sqx_bits_get(sqx_bits *b, unsigned n)
{
    uint32_t v = sqx_bits_peek(b, n);
    sqx_bits_dump(b, n);
    return v;
}

#endif /* SQX_BITIO_H */
