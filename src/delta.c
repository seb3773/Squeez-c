/* delta.c — SQX delta transforms (port of t_coder.c) */
#include "delta.h"
#include <string.h>

void sqx_delta_init(sqx_delta *d, uint32_t start_delta, uint32_t update_freq)
{
    memset(d, 0, sizeof(*d));
    d->delta = start_delta;
    d->update_freq = update_freq;
}

void sqx_delta16_init(sqx_delta16 *d, uint32_t start_delta, uint32_t update_freq)
{
    memset(d, 0, sizeof(*d));
    d->delta = start_delta;
    d->update_freq = update_freq;
}

/* Reset frequency tables only (matches SDK memset of counter sub-struct) */
static void delta_reset_freqs(sqx_delta *d)
{
    memset(d->freqs, 0, sizeof(d->freqs));
}

static void delta16_reset_freqs(sqx_delta16 *d)
{
    memset(d->freqs_high, 0, sizeof(d->freqs_high));
    memset(d->freqs_low, 0, sizeof(d->freqs_low));
}

uint8_t sqx_delta_decode8(sqx_delta *d, uint8_t c)
{
    uint8_t err[5], cur;
    uint32_t count[5], i, smallest, smallest_idx;

    switch (d->delta) {
    case 0:  cur = c; break;
    case 1:  cur = (uint8_t)(d->samps[0] - c); break;
    case 2:  cur = (uint8_t)(d->samps[1] - c); break;
    case 3:  cur = (uint8_t)(d->samps[2] - c); break;
    default: cur = (uint8_t)(d->samps[3] - c); break;
    }

    err[0] = cur;
    for (i = 1; i <= 4; i++)
        err[i] = (uint8_t)(d->samps[i - 1] - cur);

    for (i = 0; i <= 4; i++)
        d->freqs[i][err[i]]++;

    d->samps[3] = d->samps[2];
    d->samps[2] = d->samps[1];
    d->samps[1] = d->samps[0];
    d->samps[0] = cur;

    if ((d->curr_count & d->update_freq) == d->update_freq) {
        for (i = 0; i <= 4; i++) {
            count[i] = 0;
            for (uint32_t k = 0; k < 256; k++)
                if (d->freqs[i][k] > 0)
                    count[i]++;
        }
        smallest = count[0];
        smallest_idx = 0;
        for (i = 1; i <= 4; i++)
            if (count[i] < smallest) {
                smallest = count[i];
                smallest_idx = i;
            }

        if (d->delta == smallest_idx) {
            d->update_freq += d->update_freq + 1;
            d->update_freq &= 4095;
        } else {
            d->update_freq = 0x1FF;
        }
        d->delta = smallest_idx;
        delta_reset_freqs(d);
    }

    d->curr_count++;
    return cur;
}

uint16_t sqx_delta_decode16(sqx_delta16 *d, uint16_t w)
{
    uint16_t err[3], cur = 0;
    uint32_t count[3], i, smallest, smallest_idx;

    switch (d->delta) {
    case 0: cur = (uint16_t)(d->samps[0] - w); break;
    case 1: cur = (uint16_t)(d->samps[1] - w); break;
    case 2: cur = (uint16_t)(d->samps[2] - w); break;
    }

    err[0] = (uint16_t)(d->samps[0] - cur);
    err[1] = (uint16_t)(d->samps[1] - cur);
    err[2] = (uint16_t)(d->samps[2] - cur);

    for (i = 0; i <= 2; i++) {
        d->freqs_high[i][err[i] >> 8]++;
        d->freqs_low[i][err[i] & 0xFF]++;
    }

    d->samps[2] = d->samps[1];
    d->samps[1] = d->samps[0];
    d->samps[0] = cur;

    if ((d->curr_count & d->update_freq) == d->update_freq) {
        for (i = 0; i <= 2; i++) {
            count[i] = 0;
            for (uint32_t k = 0; k < 256; k++) {
                if (d->freqs_high[i][k] > 0) count[i]++;
                if (d->freqs_low[i][k] > 0) count[i]++;
            }
        }
        smallest = count[0];
        smallest_idx = 0;
        for (i = 1; i <= 2; i++)
            if (count[i] < smallest) {
                smallest = count[i];
                smallest_idx = i;
            }
        d->delta = smallest_idx;
        delta16_reset_freqs(d);
    }

    d->curr_count++;
    return cur;
}

void sqx_deconv_rx(sqx_delta *d, uint32_t num_coder, uint8_t *buf, uint32_t size)
{
    uint32_t max_count = (size / num_coder) * num_coder;
    uint32_t count = 0, idx = 0;

    while (count < max_count) {
        buf[count] = sqx_delta_decode8(&d[idx], buf[count]);
        count++;
        if (++idx == num_coder)
            idx = 0;
    }
}

void sqx_deconv_r4_16(sqx_delta16 *d, uint8_t *buf, uint32_t size)
{
    uint16_t *w = (uint16_t *)buf;
    uint32_t max_count = size >> 1;

    for (uint32_t i = 0; i < max_count; i++)
        w[i] = sqx_delta_decode16(d, w[i]);
}

void sqx_deconv_rtype(uint32_t start_delta, uint32_t coder_type,
                      uint32_t coder_num, uint32_t update_freq,
                      uint8_t *buf, uint32_t size)
{
    if (coder_num == 0 || coder_num > 4)
        return;

    if (coder_type == SQX_DCODER_16BIT) {
        sqx_delta16 d;
        sqx_delta16_init(&d, start_delta, update_freq);
        sqx_deconv_r4_16(&d, buf, size);
    } else {
        /* SDK uses 4 static structs: supports 1..4 channels */
        static sqx_delta sd[4];
        for (uint32_t i = 0; i < coder_num; i++)
            sqx_delta_init(&sd[i], start_delta, update_freq);
        sqx_deconv_rx(sd, coder_num, buf, size);
    }
}
