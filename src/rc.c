/* rc.c — SQX range coder (port of rcoder.c) */
#include "rc.h"
#include <stdlib.h>
#include <string.h>

/* `init_freq` = initial frequency for each symbol. 1.1 (rcoder.c,
 * RC_ModelInit) starts with 1; codec 2.0 starts with `incr` (freq[0] = 8,
 * thus tot_freq = 2048), creating a much flatter initial model. */
static int rc_model_setup(sqx_rcmodel *m, uint32_t nsym, uint32_t incr,
                          uint32_t max_freq, uint32_t adapt, uint32_t init_freq)
{
    uint32_t i;

    m->nsym = nsym;
    m->nsym2 = nsym / 2;
    m->nsym3 = nsym / 3;
    m->nsym4 = nsym * 5 / 22;
    m->nsym23 = nsym * 2 / 3;

    if (init_freq == 0)
        init_freq = 1;
    m->tot_freq = 0;
    for (i = 0; i < nsym; i++)
        m->tot_freq += (m->freq[i] = init_freq);

    for (m->first_freq = i = 0; i < m->nsym4; i++)
        m->first_freq += m->freq[i];
    for (m->half_freq = m->first_freq, i = m->nsym4; i < m->nsym2; i++)
        m->half_freq += m->freq[i];
    for (m->last_freq = m->half_freq, i = m->nsym2; i < m->nsym23; i++)
        m->last_freq += m->freq[i];

    m->incr = incr;
    m->max_freq = max_freq;
    m->adapt = adapt;
    m->last_cum_freq = 0;
    m->last_sym = 0;

    if (m->max_freq > SQX_RC_BOT || m->tot_freq > SQX_RC_BOT)
        return 0;
    return 1;
}

int sqx_rc_model_create(sqx_rcmodel *m, uint32_t nsym, uint32_t incr,
                        uint32_t max_freq, uint32_t adapt)
{
    return sqx_rc_model_create_ex(m, nsym, incr, max_freq, adapt, 1);
}

int sqx_rc_model_create_ex(sqx_rcmodel *m, uint32_t nsym, uint32_t incr,
                           uint32_t max_freq, uint32_t adapt,
                           uint32_t init_freq)
{
    memset(m, 0, sizeof(*m));
    m->freq = (uint32_t *)malloc(sizeof(uint32_t) * nsym);
    if (!m->freq)
        return 0;
    return rc_model_setup(m, nsym, incr, max_freq, adapt, init_freq);
}

int sqx_rc_model_init(sqx_rcmodel *m, uint32_t nsym, uint32_t incr,
                      uint32_t max_freq, uint32_t adapt)
{
    return rc_model_setup(m, nsym, incr, max_freq, adapt, 1);
}

int sqx_rc_model_init_ex(sqx_rcmodel *m, uint32_t nsym, uint32_t incr,
                         uint32_t max_freq, uint32_t adapt, uint32_t init_freq)
{
    return rc_model_setup(m, nsym, incr, max_freq, adapt, init_freq);
}

void sqx_rc_model_free(sqx_rcmodel *m)
{
    free(m->freq);
    memset(m, 0, sizeof(*m));
}

void sqx_rc_start(sqx_rc *rc, sqx_bits *b)
{
    rc->passed = rc->low = rc->code = 0;
    rc->range = 0xFFFFFFFFu;
    rc->error = 0;
    for (int i = 0; i < 4; i++)
        rc->code = (rc->code << 8) | sqx_bits_get(b, 8);
}

static void rc_decode_bits(sqx_rc *rc, sqx_rcmodel *m, sqx_bits *b,
                           uint32_t cum_freq, uint32_t sym)
{
    rc->low += cum_freq * rc->range;
    rc->range *= m->freq[sym];
    while ((rc->low ^ (rc->low + rc->range)) < SQX_RC_TOP ||
           (rc->range < SQX_RC_BOT &&
            ((rc->range = (uint32_t)(0u - rc->low) & (SQX_RC_BOT - 1)), 1))) {
        rc->code = (rc->code << 8) | sqx_bits_get(b, 8);
        rc->range <<= 8;
        rc->low <<= 8;
    }
}

int sqx_rc_decode_symbol(sqx_rc *rc, sqx_rcmodel *m, sqx_bits *b)
{
    uint32_t i, sym, cum_freq, rfreq;

    /* Safety guard against range smaller than total frequency (zero division) */
    if (rc->range < m->tot_freq) {
        rc->error = 1;
        return -1;
    }
    rfreq = (rc->code - rc->low) / (rc->range /= m->tot_freq);
    if (rfreq >= m->tot_freq) {
        rc->error = 1;
        rc->range = 0xFFFFFFFFu;
        return -1;
    }

    if (m->last_cum_freq > rfreq) {
        if (rfreq > m->half_freq || (m->last_sym <= m->nsym23 && rfreq >= m->first_freq)) {
            for (sym = m->last_sym - 1, cum_freq = m->last_cum_freq - m->freq[sym];;) {
                if (cum_freq <= rfreq)
                    break;
                cum_freq -= m->freq[--sym];
            }
            m->last_cum_freq = cum_freq;
            m->last_sym = sym;
        } else {
            for (sym = 0, cum_freq = m->freq[0];;) {
                if (cum_freq > rfreq)
                    break;
                cum_freq += m->freq[++sym];
            }
            cum_freq -= m->freq[sym];
            if (m->adapt)
                m->last_cum_freq += m->incr;
        }
    } else {
        if (rfreq < m->half_freq || (m->last_sym >= m->nsym3 && rfreq <= m->last_freq)) {
            for (sym = m->last_sym, cum_freq = m->last_cum_freq + m->freq[sym];;) {
                if (cum_freq > rfreq)
                    break;
                cum_freq += m->freq[++sym];
            }
            cum_freq -= m->freq[sym];
            m->last_cum_freq = cum_freq;
            m->last_sym = sym;
        } else {
            for (sym = m->nsym - 1, cum_freq = m->tot_freq - m->freq[sym];;) {
                if (cum_freq <= rfreq)
                    break;
                cum_freq -= m->freq[--sym];
            }
        }
    }

    rc_decode_bits(rc, m, b, cum_freq, sym);

    if (m->adapt) {
        m->freq[sym] += m->incr;
        if (sym < m->nsym23) {
            m->last_freq += m->incr;
            if (sym < m->nsym2) {
                m->half_freq += m->incr;
                if (sym < m->nsym4)
                    m->first_freq += m->incr;
            }
        }
        m->tot_freq += m->incr;
        if (m->tot_freq > m->max_freq) {
            for (i = m->tot_freq = 0; i < m->nsym; i++)
                m->tot_freq += (m->freq[i] -= (m->freq[i] >> 1));
            m->half_freq /= 2;
            m->first_freq /= 2;
            m->last_freq /= 2;
            m->last_cum_freq = 0;
            m->last_sym = 0;
        }
    }

    return (int)sym;
}

/* ---------------------------------------------------- Encoder Interface */

int sqx_rc_enc_init(sqx_rc_enc *e, sqx_bitw *w)
{
    memset(e, 0, sizeof(*e));
    e->w = w;
    e->low = 0;
    e->range = 0xFFFFFFFFu;
    return 1;
}

static void rc_enc_norm(sqx_rc_enc *e)
{
    while ((e->low ^ (e->low + e->range)) < SQX_RC_TOP ||
           (e->range < SQX_RC_BOT &&
            ((e->range = (uint32_t)(0u - e->low) & (SQX_RC_BOT - 1)), 1))) {
        sqx_bitw_put(e->w, e->low >> 24, 8);
        e->window = (e->window << 8) | (e->low >> 24);
        e->range <<= 8;
        e->low <<= 8;
    }
}

int sqx_rc_enc_symbol(sqx_rc_enc *e, sqx_rcmodel *m, unsigned sym)
{
    uint32_t cum = 0, rfreq, q;
    uint32_t i;

    if (m->tot_freq == 0)
        return 0;
    for (i = 0; i < sym; i++)
        cum += m->freq[i];

    q = e->range / m->tot_freq;                    /* "GetFreq" */
    rfreq = (e->window - e->low) / q;

    /* Exact mirror of decoder search branch */
    if (m->last_cum_freq > rfreq) {
        if (rfreq > m->half_freq ||
            (m->last_sym <= m->nsym23 && rfreq >= m->first_freq)) {
            m->last_cum_freq = cum;                /* from top */
            m->last_sym = sym;
        } else {
            if (m->adapt)                          /* from start */
                m->last_cum_freq += m->incr;
        }
    } else {
        if (rfreq < m->half_freq ||
            (m->last_sym >= m->nsym3 && rfreq <= m->last_freq)) {
            m->last_cum_freq = cum;                /* from lastSym */
            m->last_sym = sym;
        }
    }

    if (q == 0)
        return 0;
    e->range = q * m->freq[sym];
    e->low += cum * q;
    rc_enc_norm(e);

    if (m->adapt) {
        m->freq[sym] += m->incr;
        if (sym < m->nsym23) {
            m->last_freq += m->incr;
            if (sym < m->nsym2) {
                m->half_freq += m->incr;
                if (sym < m->nsym4)
                    m->first_freq += m->incr;
            }
        }
        m->tot_freq += m->incr;
        if (m->tot_freq > m->max_freq) {
            for (i = m->tot_freq = 0; i < m->nsym; i++)
                m->tot_freq += (m->freq[i] -= (m->freq[i] >> 1));
            m->half_freq /= 2;
            m->first_freq /= 2;
            m->last_freq /= 2;
            m->last_cum_freq = 0;
            m->last_sym = 0;
        }
    }
    return 1;
}

void sqx_rc_enc_flush(sqx_rc_enc *e)
{
    unsigned i;

    for (i = 0; i < 4; i++) {
        sqx_bitw_put(e->w, e->low >> 24, 8);
        e->window = (e->window << 8) | (e->low >> 24);
        e->low <<= 8;
    }
}
