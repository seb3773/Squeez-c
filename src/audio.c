/* audio.c — SQX audio decoder (method 5), port of cx_audio.c (SDK 2.06)
 * and reverse-engineered SQX 2.0 audio engine from sqx20u.dll.
 *
 * Container format is identical in 1.1 and 2.0 (both WAV archives in corpus
 * start with the same 18-byte audio header).
 */
#include "audio.h"

#include "bitio.h"
#include "bitw.h"
#include "huff.h"
#include "rc.h"
#include "sqx.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SQX_AUDIO_HDR_BYTES  18u     /* SQX_AUDIO_HEAD as serialized in stream */
#define SQX_AUDIO_ANC        0x102u  /* 258 symbols (CTabAudio) */
#define SQX_AUDIO_FRAME_MAX  384u    /* SAMP_FRAME_MAX */

/* Frame header bits for 16-bit audio (cx_audio.c) */
#define SQX_AUDIO_HEAD_T_BITS      1u
#define SQX_AUDIO_CHANNEL_DEC_BITS 2u
#define SQX_AUDIO_PRED_BITS        3u
#define SQX_AUDIO_SHIFT_BITS       4u
#define SQX_AUDIO_MAX_FRAME_BITS   11u
#define SQX_AUDIO_MAX_8CODE_BITS   14u
#define SQX_AUDIO_FRAME_TYPE_LAST  1u

/* DecodeAudioDelta: AAC-style 6 predictors (cx_audio.c, AUDIO_DELTA_STRUCT) */
typedef struct {
    uint32_t freqs[6][256];
    uint8_t  samps[4];
    uint32_t delta;
    uint32_t update_freq;
    uint32_t curr_count;
} sqx_aud_delta;

static void aud_delta_init(sqx_aud_delta *d, uint32_t start, uint32_t update)
{
    memset(d, 0, sizeof(*d));
    d->delta = start;
    d->update_freq = update;
}

/* Returns reconstructed byte. `c` = delta read from stream. */
static uint8_t aud_delta_decode(sqx_aud_delta *d, uint8_t c)
{
    uint32_t i, count[6], smallest, smallest_idx;
    uint8_t cur, err[6];

    /* Predictors are computed in integer then truncated to 8-bit (UBYTE) */
    switch (d->delta) {
    case 0:  cur = c; break;
    case 1:  cur = (uint8_t)(d->samps[0] - c); break;
    case 2:  cur = (uint8_t)((2 * d->samps[0]) - d->samps[1] - c); break;
    case 3:  cur = (uint8_t)((3 * d->samps[0]) - (3 * d->samps[1]) +
                             d->samps[2] - c); break;
    case 4:  cur = (uint8_t)((((4 * d->samps[0]) - (3 * d->samps[1]) +
                                d->samps[2]) >> 1) - c); break;
    default: cur = (uint8_t)((4 * d->samps[0]) + (6 * d->samps[1]) -
                             (4 * d->samps[2]) + d->samps[3] - c); break;
    }

    err[0] = (uint8_t)cur;
    err[1] = (uint8_t)(d->samps[0] - cur);
    err[2] = (uint8_t)(((2 * d->samps[0]) - d->samps[1]) - cur);
    err[3] = (uint8_t)(((3 * d->samps[0]) - (3 * d->samps[1]) + d->samps[2]) - cur);
    err[4] = (uint8_t)(((4 * d->samps[0] - 3 * d->samps[1] + d->samps[2]) >> 1) - cur);
    err[5] = (uint8_t)(((4 * d->samps[0]) + (6 * d->samps[1]) -
                        (4 * d->samps[2]) + d->samps[3]) - cur);

    for (i = 0; i < 6; i++)
        d->freqs[i][err[i]]++;

    d->samps[3] = d->samps[2];
    d->samps[2] = d->samps[1];
    d->samps[1] = d->samps[0];
    d->samps[0] = cur;

    if ((d->curr_count & d->update_freq) == d->update_freq) {
        for (i = 0; i < 6; i++) {
            count[i] = 0;
            for (uint32_t k = 0; k < 256; k++)
                if (d->freqs[i][k] > 0)
                    count[i]++;
        }
        smallest = count[0];
        smallest_idx = 0;
        for (i = 1; i < 6; i++)
            if (count[i] < smallest) {
                smallest = count[i];
                smallest_idx = i;
            }
        d->delta = smallest_idx;
        memset(d->freqs, 0, sizeof(d->freqs));   /* only frequency tables reset */
    }

    d->curr_count++;
    return cur;
}

/* ER_STRUCT + ReadData/DeSample (16-bit samples) */
typedef struct {
    int32_t  start_v;
    int32_t  lv[8];
    uint32_t pred_t;
} sqx_aud_er;

static void aud_read_data(sqx_aud_er *e, sqx_bits *b, uint32_t size, int32_t *out)
{
    uint32_t i;
    int32_t  shift;
    uint32_t v;

    e->pred_t = sqx_bits_get(b, SQX_AUDIO_PRED_BITS);
    shift = (int32_t)sqx_bits_get(b, SQX_AUDIO_SHIFT_BITS);

    for (i = 0; i < size; i++) {
        uint32_t sign = sqx_bits_get(b, 1);
        uint32_t cnt = 0;
        uint32_t res = sqx_bits_get(b, 1);

        while (res == 0) {                        /* unary code for bit length */
            cnt++;
            res = sqx_bits_get(b, 1);
        }

        if (shift > 0)
            v = sqx_bits_get(b, (unsigned)shift);
        else
            v = 0;
        if (cnt > 0)
            v |= cnt << shift;

        out[i] = (sign == 1) ? -(int32_t)v : (int32_t)v;
    }
}

static void aud_desample(sqx_aud_er *e, const int32_t *data, uint32_t size, int32_t *out)
{
    int32_t le0t = 0, le1t = 0, le1tt = 0, le2t = 0, le2tt = 0, le4t = 0, le4tt = 0;
    uint32_t pred = e->pred_t, i;

    for (i = 0; i < size; i++) {
        int32_t base = data[i], sample, diff, le;

        switch (pred) {
        case 0:
            le = base;
            diff = 0;
            if (le0t < 0) { diff = abs(le0t) >> 4; diff = -diff; }
            else if (le0t > 0) diff = abs(le0t) >> 4;
            le += diff;
            le0t = base;
            sample = le + e->start_v;
            break;
        case 1:
            le = base;
            diff = 0;
            if (le1t < 0) {
                diff = abs(le1t) >> 1;
                if (le1tt < 0) diff += (diff >> 1);
                diff = -diff;
            } else if (le1t > 0) {
                diff = abs(le1t) >> 1;
                if (le1tt > 0) diff += (diff >> 1);
            }
            le += diff;
            sample = le + e->lv[0];
            le1tt = le1t;
            le1t = base;
            break;
        case 2:
            le = base;
            diff = 0;
            if (le2t < 0) {
                diff = abs(le2t) >> 1;
                if (le2tt < 0) diff += (diff >> 1);
                diff = -diff;
            } else if (le2t > 0) {
                diff = abs(le2t) >> 1;
                if (le2tt > 0) diff += (diff >> 1);
            }
            le += diff;
            sample = le + ((2 * e->lv[0]) - e->lv[1]);
            le2tt = le2t;
            le2t = base;
            break;
        case 3:
            sample = base + ((3 * e->lv[0]) - (3 * e->lv[1]) + e->lv[2]);
            break;
        case 4:
            le = base;
            diff = 0;
            if (le4t < 0) {
                diff = abs(le4t) >> 1;
                if (le4tt < 0) diff += (diff >> 1);
                diff = -diff;
            } else if (le4t > 0) {
                diff = abs(le4t) >> 1;
                if (le4tt > 0) diff += (diff >> 1);
            }
            le += diff;
            sample = le + ((4 * e->lv[0] - 3 * e->lv[1] + e->lv[2]) >> 1);
            le4tt = le4t;
            le4t = base;
            break;
        default:
            sample = base;
            break;
        }

        e->lv[3] = e->lv[2];
        e->lv[2] = e->lv[1];
        e->lv[1] = e->lv[0];
        e->lv[0] = e->start_v = sample;
        out[i] = sample;
    }
}

/* ------------------------------------------------------------ output */

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
} aud_out;

static void aud_put8(aud_out *o, uint8_t v)
{
    if (o->len < o->cap) o->buf[o->len++] = v;
}

static void aud_put16(aud_out *o, int16_t v)
{
    aud_put8(o, (uint8_t)(v & 0xFF));
    aud_put8(o, (uint8_t)((uint16_t)v >> 8));
}

/* GetExtraData: copy n raw bytes directly to output */
static void aud_extra(aud_out *o, sqx_bits *b, int64_t n)
{
    while (n-- > 0)
        aud_put8(o, (uint8_t)sqx_bits_get(b, 8));
}

/* ------------------------------------------------------------ 8-bit codecs */

static void aud_8bit(sqx_bits *b, aud_out *o, uint32_t channels,
                     uint32_t raw_size, uint32_t arcer_ver)
{
    sqx_huff bitct, tab[2];
    sqx_aud_delta delta[2];
    uint32_t bt_block = 0, idx = 0, two = 0;

    if (!sqx_huff_alloc(&bitct, SQX_BCODES, SQX_BCODE_BITS) ||
        !sqx_huff_alloc(&tab[0], SQX_AUDIO_ANC, SQX_LCODE_BITS) ||
        !sqx_huff_alloc(&tab[1], SQX_AUDIO_ANC, SQX_LCODE_BITS)) {
        sqx_huff_free(&bitct); sqx_huff_free(&tab[0]); sqx_huff_free(&tab[1]);
        return;
    }

    aud_delta_init(&delta[0], 0, channels == 2 ? 511 : 1023);
    aud_delta_init(&delta[1], 0, channels == 2 ? 511 : 1023);

    while (raw_size > 0) {
        uint32_t sym;

        if (bt_block == 0) {
            bt_block = sqx_bits_get(b, SQX_AUDIO_MAX_8CODE_BITS);
            if (channels == 2) {
                two = sqx_bits_get(b, 1);
                idx = 0;
            }
            for (uint32_t i = 0; i < bitct.num_code; i++)
                bitct.lens[i] = (uint8_t)sqx_bits_get(b, 4);
            if (!sqx_huff_generate(&bitct))
                return;
            if (!sqx_read_one_tab_tree(&tab[0], b, &bitct))
                return;
            if (channels == 2 && two && !sqx_read_one_tab_tree(&tab[1], b, &bitct))
                return;
        }

        sym = sqx_huff_get(&tab[0], b);
        bt_block--;
        aud_put8(o, aud_delta_decode(&delta[channels == 2 ? idx : 0], (uint8_t)sym));
        if (channels == 2 && !two)
            idx ^= 1;
        raw_size--;

        if (channels == 2 && two && raw_size > 0) {
            sym = sqx_huff_get(&tab[1], b);
            aud_put8(o, aud_delta_decode(&delta[1], (uint8_t)sym));
            raw_size--;
        }
    }

    sqx_huff_free(&bitct);
    sqx_huff_free(&tab[0]);
    sqx_huff_free(&tab[1]);
    (void)arcer_ver;
}

/* ------------------------------------------------------------ 16-bit codecs */

static int aud_16bit(sqx_bits *b, aud_out *o, uint32_t channels, uint32_t raw_size)
{
    int32_t *lbuf = (int32_t *)malloc(SQX_AUDIO_FRAME_MAX * sizeof(int32_t) * 2);
    int32_t *rbuf = (int32_t *)malloc(SQX_AUDIO_FRAME_MAX * sizeof(int32_t) * 2);
    int32_t *lsmp = (int32_t *)malloc(SQX_AUDIO_FRAME_MAX * sizeof(int32_t));
    int32_t *rsmp = (int32_t *)malloc(SQX_AUDIO_FRAME_MAX * sizeof(int32_t));
    sqx_aud_er el, er;
    uint32_t frame = 0;
    int ok = 0;

    if (!lbuf || !rbuf || !lsmp || !rsmp)
        goto out;

    memset(&el, 0, sizeof(el));
    memset(&er, 0, sizeof(er));

    while (raw_size >= 4) {
        uint32_t size, icd = 0;
        uint32_t head = sqx_bits_get(b, SQX_AUDIO_HEAD_T_BITS);

        if (channels == 2)
            icd = sqx_bits_get(b, SQX_AUDIO_CHANNEL_DEC_BITS);
        else
            sqx_bits_dump(b, SQX_AUDIO_CHANNEL_DEC_BITS);   /* ignored in mono */

        if (head == SQX_AUDIO_FRAME_TYPE_LAST)
            size = sqx_bits_get(b, SQX_AUDIO_MAX_FRAME_BITS);
        else
            size = SQX_AUDIO_FRAME_MAX;

        /* Legacy SDK never reads more than SAMP_FRAME_MAX samples per frame
         * (buffer capacity); a larger value indicates a corrupt stream. */
        if (size == 0 || size > SQX_AUDIO_FRAME_MAX)
            goto out;

        raw_size -= size * (channels == 2 ? 4u : 2u);

        aud_read_data(&el, b, size, lbuf);
        aud_desample(&el, lbuf, size, lsmp);
        if (getenv("SQX_AUDIO_TRACE"))
            fprintf(stderr, "[audio] frame %u: head=%u size=%u pred=%u pos=%llu "
                            "smp0=%d smp1=%d\n",
                    frame, head, size, el.pred_t,
                    (unsigned long long)(b->wpos * 32 + b->bits),
                    size ? lsmp[0] : 0, size > 1 ? lsmp[1] : 0);
        frame++;

        if (channels == 1) {
            for (uint32_t i = 0; i < size; i++)
                aud_put16(o, (int16_t)lsmp[i]);
            continue;
        }

        aud_read_data(&er, b, size, rbuf);
        aud_desample(&er, rbuf, size, rsmp);

        for (uint32_t i = 0; i < size; i++) {
            int32_t l = lsmp[i], r = rsmp[i];

            if (icd == 1) {
                r -= (l >> 1);
                l += r;
            } else if (icd == 2) {
                r -= (l >> 1);
                l += (r >> 1);
            }
            aud_put16(o, (int16_t)l);
            aud_put16(o, (int16_t)r);
        }
    }
    ok = 1;

out:
    free(lbuf); free(rbuf); free(lsmp); free(rsmp);
    return ok;
}

/* ------------------------------------------- Audio 2.0 codec (16-bit) */
/*
 * SQX 2.0 replaces 1.1 polynomial prediction with 4608-sample frames
 * and a per-channel packet specifying an adaptive FIR filter or polynomial predictor:
 *
 *   mode(1) == 1 -> count(5 bits, +1 coefficients); width(6); count times
 *                   (sign(1) + width-1 bits) = FIR coefficients;
 *                   shift(4); f94(1); flag(1)
 *   mode(1) == 0 -> predictor(3); flag(1) (polynomial predictors 0..3, plus
 *                   adaptive predictor 4)
 *
 * `flag` selects the residual encoding body:
 *   1 -> Adaptive Rice coding (0x1001d7d0): sign, unary prefix,
 *        k-5 least significant bits; k/accumulator adapt dynamically
 *        (accum += |v| - accum/32, thresholds 1<<k and 1<<(k+1))
 *   0 -> Fixed width (0x1001da70): width(4 bits), then for each value:
 *        sign, unary prefix, width bits
 *
 * Residuals then pass through cascading synthesis filters (0x1001e370..):
 *   mode 0 : residuals -> polynomial predictor -> samples
 *   mode 1 :
 *     A (if f94) : v[i] += v[i-1] >> 1          (pre-update v[i-1])
 *     C          : copy first `count` samples, then
 *                  v[i] = res[i] + (sum_j coef[j]*v[i-1-j]) >> shift
 *     B          : v[i] += (3 * v[i-1]) >> 2
 *
 * Reverse-engineered from sqx20u.dll and verified against the legacy test corpus.
 */

#define SQX_AUD2_FRAME_MAX  0x1200u   /* 4608 samples per frame */
#define SQX_AUD2_COEF_MAX   32u       /* 5 bits + 1 */
#define SQX_AUD2_K_RICE     14u       /* initial width for adaptive Rice */
#define SQX_AUD2_UNARY_MAX  0x100000u /* guard threshold against infinite loops */

/* Channel packet (mode 1 or mode 0) */
typedef struct {
    uint32_t count;                   /* FIR coefficient count */
    uint32_t width;                   /* coefficient bit width */
    uint32_t shift;
    uint32_t pred;                    /* mode 0 only */
    int32_t  coef[SQX_AUD2_COEF_MAX];
    int      f94;                     /* filter A integrator */
    int      flag;                    /* 1 = adaptive Rice body */
    int      mode0;                   /* mode 0: polynomial predictors */
} aud2_packet;

/* Fixed-width frame body (0x1001da70) */
static int aud2_res_fixed(sqx_bits *b, uint32_t size, int32_t *out)
{
    uint32_t width = sqx_bits_get(b, 4);

    if (getenv("SQX_AUDIO_TRACE"))
        fprintf(stderr, "          fixed body: w=%u pos=%llu\n", width,
                (unsigned long long)(b->wpos * 32 + b->bits));
    if (width > 32)
        return 0;
    for (uint32_t i = 0; i < size; i++) {
        uint32_t sign = sqx_bits_get(b, 1), unary = 0, v;

        while (sqx_bits_peek(b, 1) == 0) {
            sqx_bits_get(b, 1);
            if (++unary > SQX_AUD2_UNARY_MAX)
                return 0;
        }
        sqx_bits_get(b, 1);                        /* terminator */
        v = unary << width;
        if (width)
            v |= sqx_bits_get(b, width);
        out[i] = sign ? -(int32_t)v : (int32_t)v;
    }
    return 1;
}

/* Adaptive Rice frame body (0x1001d7d0). */
static int aud2_res_rice(sqx_bits *b, uint32_t size, int32_t *out)
{
    uint32_t k = SQX_AUD2_K_RICE;
    uint32_t accum = 1u << SQX_AUD2_K_RICE;

    for (uint32_t i = 0; i < size; i++) {
        uint32_t sign = sqx_bits_get(b, 1), unary = 0, n, v;

        while (sqx_bits_peek(b, 1) == 0) {
            sqx_bits_get(b, 1);
            if (++unary > SQX_AUD2_UNARY_MAX)
                return 0;
        }
        sqx_bits_get(b, 1);                        /* terminator */
        n = k > 5 ? k - 5 : 0;                     /* least significant bits */
        v = n ? (sqx_bits_get(b, n) | (unary << n)) : unary;
        out[i] = sign ? -(int32_t)v : (int32_t)v;

        accum += v - (accum >> 5);                 /* |v| = v here */
        if (k > 4 && accum < (1u << k))
            k--;
        else if (k < 31 && accum >= (1u << (k + 1)))
            k++;
    }
    return 1;
}

/* |v| matching legacy DLL (test/jge/neg: INT32_MIN remains INT32_MIN). */
static int32_t aud2_abs32(int32_t v)
{
    return v < 0 ? (int32_t)(0u - (uint32_t)v) : v;
}

/* Mode 0 adaptive predictor (0x1001e210), last jump table case.
 *
 * Three exponential magnitude estimators, indexed by position within packet
 * (`i & 15`), decide per sample how many times the residual is integrated:
 * `esi` = residual, `ebx` = running sum, `edi` = running delta.
 * Verified against DLL via direct emulation (tools/emu.py).
 *
 * The three accumulators `acc14`/`acc1c`/`acc20` are fed back into
 * `eax`/`ebp`/`ecx` in the next cycle to drive predictor selection. */
static void aud2_predict4(uint32_t size, const int32_t *res, int32_t *out)
{
    int32_t a1[16], a2[16], a3[16];
    int32_t acc14 = 0, acc1c = 0, acc20 = 0;
    int32_t ebx = 0, edi = 0;                  /* running sum and delta */
    int32_t eax_r = 0, ebp_r = 0, ecx_r = 0;

    memset(a1, 0, sizeof(a1));
    memset(a2, 0, sizeof(a2));
    memset(a3, 0, sizeof(a3));

    for (uint32_t i = 0; i < size; i++) {
        uint32_t idx = i & 15u;
        int32_t esi, edx, m;

        if (i)
            eax_r = acc14, ebp_r = acc1c, ecx_r = acc20;
        esi = res[i];
        if (eax_r < ebp_r && eax_r < ecx_r) {          /* 0x1001e286 */
            eax_r = (int32_t)((uint32_t)esi - (uint32_t)ebx);
            ecx_r = esi;
            ebx = esi;
            edx = (int32_t)((uint32_t)eax_r - (uint32_t)edi);
            edi = eax_r;
        } else if (ebp_r < ecx_r) {                    /* 0x1001e29a */
            edx = (int32_t)((uint32_t)esi - (uint32_t)edi);
            ecx_r = (int32_t)((uint32_t)esi + (uint32_t)ebx);
            eax_r = esi;
            ebx = ecx_r;
            edi = esi;
        } else {                                       /* 0x1001e2a9 */
            eax_r = (int32_t)((uint32_t)esi + (uint32_t)edi);
            edx = esi;
            edi = eax_r;
            ecx_r = (int32_t)((uint32_t)eax_r + (uint32_t)ebx);
            ebx = ecx_r;
        }
        out[i] = ecx_r;

        m = aud2_abs32(ecx_r);
        acc14 = (int32_t)((uint32_t)acc14 - (uint32_t)a1[idx] + (uint32_t)m);
        a1[idx] = (int32_t)((uint32_t)a1[idx] + (uint32_t)m) >> 1;
        m = aud2_abs32(eax_r);
        acc1c = (int32_t)((uint32_t)acc1c - (uint32_t)a2[idx] + (uint32_t)m);
        a2[idx] = (int32_t)((uint32_t)a2[idx] + (uint32_t)m) >> 1;
        m = aud2_abs32(edx);
        acc20 = (int32_t)((uint32_t)acc20 - (uint32_t)a3[idx] + (uint32_t)m);
        a3[idx] = (int32_t)((uint32_t)a3[idx] + (uint32_t)m) >> 1;
    }
}

/* Mode 0 polynomial predictors (0x1001e120 .. 0x1001e210), state reset on each
 * call (one frame per channel). Uses 32-bit wrapping arithmetic matching the DLL. */
static int aud2_predict0(uint32_t pred, uint32_t size, const int32_t *res,
                         int32_t *out)
{
    uint32_t i;

    switch (pred) {
    case 0:                                        /* 0x1001e120: pass-through copy */
        for (i = 0; i < size; i++)
            out[i] = res[i];
        return 1;
    case 1: {                                      /* 0x1001e150: integrator */
        uint32_t s = 0;
        for (i = 0; i < size; i++) {
            s += (uint32_t)res[i];
            out[i] = (int32_t)s;
        }
        return 1;
    }
    case 2: {                                      /* 0x1001e180 */
        uint32_t p1 = 0, p2 = 0;
        for (i = 0; i < size; i++) {
            uint32_t v = (uint32_t)res[i] + 2u * p1 - p2;
            out[i] = (int32_t)v;
            p2 = p1;
            p1 = v;
        }
        return 1;
    }
    case 3: {                                      /* 0x1001e1c0 */
        uint32_t p1 = 0, p2 = 0, p3 = 0;
        for (i = 0; i < size; i++) {
            uint32_t v = (uint32_t)res[i] + 3u * (p1 - p2) + p3;
            out[i] = (int32_t)v;
            p3 = p2;
            p2 = p1;
            p1 = v;
        }
        return 1;
    }
    case 4:                                        /* 0x1001e210: adaptive */
        aud2_predict4(size, res, out);
        return 1;
    default:
        return 0;                                  /* predictors 5..7: unknown */
    }
}

static uint32_t aud_rd16le(const uint8_t *p);

static int32_t aud2_sar64v(int64_t v, uint32_t s);

/* Read channel packet. Returns 1 on success, -1 on corrupt stream. */
static int aud2_read_packet(sqx_bits *b, aud2_packet *p)
{
    memset(p, 0, sizeof(*p));
    if (sqx_bits_get(b, 1) == 0) {                 /* mode 0: polynomial predictors */
        p->mode0 = 1;
        p->pred = sqx_bits_get(b, 3);
        p->flag = (int)sqx_bits_get(b, 1);
        return 1;
    }
    p->count = sqx_bits_get(b, 5) + 1;
    p->width = sqx_bits_get(b, 6);
    if (p->width > 33)
        return -1;
    for (uint32_t i = 0; i < p->count; i++) {
        uint32_t sign = sqx_bits_get(b, 1);
        uint32_t v = p->width > 1 ? sqx_bits_get(b, p->width - 1) : 0;

        p->coef[i] = sign ? -(int32_t)v : (int32_t)v;
    }
    p->shift = sqx_bits_get(b, 4);
    p->f94 = (int)sqx_bits_get(b, 1);
    p->flag = (int)sqx_bits_get(b, 1);
    return 1;
}

/* Filters A/C/B (0x1001e370..0x1001e460). `res` is modified in-place. */
static void aud2_filters(const aud2_packet *p, uint32_t size, int32_t *res,
                         int32_t *out)
{
    uint32_t i, j, n = p->count > size ? size : p->count;
    int32_t prev;

    if (p->f94) {                                  /* A (0x1001e3a0) */
        prev = 0;
        for (i = 0; i < size; i++) {
            int32_t cur = res[i];
            res[i] = (int32_t)((uint32_t)cur + (uint32_t)(prev >> 1));
            prev = cur;
        }
    }

    for (i = 0; i < n; i++)                       /* C: copy first n samples */
        out[i] = res[i];
    for (i = n; i < size; i++) {
        int64_t acc = 0;

        for (j = 0; j < n; j++)                   /* 64-bit sum (x86 32x32 -> 64 imul) then sar by 32 */
            acc += (int64_t)p->coef[j] * out[i - 1 - j];
        out[i] = (int32_t)((uint32_t)res[i] +
                           (uint32_t)aud2_sar64v(acc, p->shift));
    }

    prev = 0;                                     /* B (0x1001e370) */
    for (i = 0; i < size; i++) {
        int32_t cur = out[i];
        int32_t d = (int32_t)(3u * (uint32_t)prev) >> 2;
        out[i] = (int32_t)((uint32_t)cur + (uint32_t)d);
        prev = cur;
    }
}

static int aud_16bit_v2(sqx_bits *b, aud_out *o, uint32_t channels,
                        uint32_t raw_size, int *unsupported)
{
    int32_t *res[2] = { NULL, NULL }, *smp[2] = { NULL, NULL };
    aud2_packet pk[2];
    uint32_t frame = 0;
    int ok = 0;

    for (uint32_t c = 0; c < 2; c++) {
        res[c] = (int32_t *)malloc(SQX_AUD2_FRAME_MAX * sizeof(int32_t));
        smp[c] = (int32_t *)malloc(SQX_AUD2_FRAME_MAX * sizeof(int32_t));
        if (!res[c] || !smp[c])
            goto out;
    }

    while (raw_size >= 4) {
        uint32_t head = sqx_bits_get(b, 1);
        uint32_t icd = sqx_bits_get(b, SQX_AUDIO_CHANNEL_DEC_BITS);
        uint32_t size = head == SQX_AUDIO_FRAME_TYPE_LAST
                        ? sqx_bits_get(b, 13) : SQX_AUD2_FRAME_MAX;

        if (size == 0 || size > SQX_AUD2_FRAME_MAX)
            goto out;
        if ((uint64_t)size * channels * 2u > raw_size)   /* truncated stream */
            goto out;
        raw_size -= size * (channels == 2 ? 4u : 2u);

        int tr = getenv("SQX_AUDIO_TRACE") != NULL;
        if (tr)
            fprintf(stderr, "[audio2] frame %u: head=%u icd=%u size=%u pos=%llu\n",
                    frame, head, icd, size,
                    (unsigned long long)(b->wpos * 32 + b->bits));
        for (uint32_t c = 0; c < channels; c++) {
            if (aud2_read_packet(b, &pk[c]) < 0)
                goto out;
            if (tr)
                fprintf(stderr,
                        "[audio2]   channel %u: mode=%u pred=%u count=%u "
                        "width=%u shift=%u f94=%d flag=%d pos=%llu\n",
                        c, pk[c].mode0 ? 0u : 1u, pk[c].pred, pk[c].count,
                        pk[c].width, pk[c].shift, pk[c].f94, pk[c].flag,
                        (unsigned long long)(b->wpos * 32 + b->bits));
            if (pk[c].flag ? !aud2_res_rice(b, size, res[c])
                           : !aud2_res_fixed(b, size, res[c]))
                goto out;
            if (tr)
                fprintf(stderr,
                        "[audio2]   channel %u: res[%s] end pos=%llu\n",
                        c, pk[c].flag ? "rice" : "fixed",
                        (unsigned long long)(b->wpos * 32 + b->bits));
            if (pk[c].mode0) {
                if (!aud2_predict0(pk[c].pred, size, res[c], smp[c])) {
                    *unsupported = 1;
                    goto out;
                }
            } else {
                aud2_filters(&pk[c], size, res[c], smp[c]);
            }
        }
        frame++;

        if (channels == 1) {
            for (uint32_t i = 0; i < size; i++)
                aud_put16(o, (int16_t)smp[0][i]);
        } else {
            /* SQX 2.0 stereo interleaver (0x1001e680, jump table 0x1001e834),
             * distinct from SQX 1.1 SplitChannels:
             *   0 -> [L, R]          1 -> [L, L-R]        2 -> [L+R, L]
             *   3 -> [L + R/2, L - R/2] (32-bit, rounded by bit 0 of R)
             * Cases 1 and 2 operate on 16-bit values, case 3 on 32-bit channel values. */
            for (uint32_t i = 0; i < size; i++) {
                int32_t l = smp[0][i], r = smp[1][i];
                int16_t l16 = (int16_t)l, r16 = (int16_t)r, o0, o1;

                switch (icd) {
                case 1:
                    o0 = l16;
                    o1 = (int16_t)(uint16_t)(l16 - r16);
                    break;
                case 2:
                    o0 = (int16_t)(uint16_t)(l16 + r16);
                    o1 = l16;
                    break;
                case 3: {
                    int32_t c = (int32_t)(2u * (uint32_t)l + (uint32_t)(r & 1));
                    int32_t t = (int32_t)((uint32_t)r + (uint32_t)c);
                    int32_t u = (int32_t)((uint32_t)c - (uint32_t)r);

                    o0 = (int16_t)(t >> 1);          /* sar */
                    o1 = (int16_t)(u >> 1);
                    break;
                }
                default:
                    o0 = l16;
                    o1 = r16;
                    break;
                }
                aud_put16(o, o0);
                aud_put16(o, o1);
            }
        }
    }
    ok = 1;

out:
    for (uint32_t c = 0; c < 2; c++) {
        free(res[c]);
        free(smp[c]);
    }
    return ok;
}

/* ------------------------------------------------- Codec 2.0, 8-bit channels */

/* Algorithm reverse-engineered from sqx20u.dll disassembly:
 *
 *   18-byte header | skip bytes | samples | tail bytes
 *
 * Bytes are encoded in a single symbol stream: a range coder (identical to
 * the MM block range coder) feeds, per channel, 256 contexts of 17 models of
 * 16 symbols each (0x100232e0); context is the last decoded symbol, high nibble
 * comes from model 0 and low nibble from model `high + 1` (0x100226d0).
 * Each byte then passes through an adaptive 6-case delta predictor (0x1001f4f0).
 * Channels alternate byte-by-byte. */

#define SQX_AUD2_8_NSYM   16u     /* symbols per model (one nibble) */
#define SQX_AUD2_8_INCR   4u      /* increment = initial frequency */
#define SQX_AUD2_8_MAXF   0x800u  /* rescaling threshold */
#define SQX_AUD2_8_SUB    17u     /* 1 high model, 16 low models */
#define SQX_AUD2_8_CTX    256u    /* contexts = possible symbols */
#define SQX_AUD2_8_UPDATE 0x240u  /* predictor re-selection threshold */

/* Adaptive 8-bit delta predictor (0x1001f4f0 + tail 0x1001f5c0). */
typedef struct {
    uint32_t freqs[6][256];
    uint8_t  s[4];                    /* 4 previous bytes (s0 = most recent) */
    uint32_t sel;                     /* [obj+0x2028] */
    uint32_t count;                   /* [obj+0x202c] */
} aud2_delta8;

/* One context: 17 models of 16 symbols, frequencies in shared block. */
typedef struct {
    sqx_rcmodel m[SQX_AUD2_8_SUB];
} aud2_ctx8;

typedef struct {
    aud2_ctx8 *ctx;                   /* SQX_AUD2_8_CTX contexts */
    uint32_t  *freq;                  /* frequency block (17 x 16 / context) */
} aud2_chan8;

static uint8_t aud2_delta8_decode(aud2_delta8 *d, uint32_t sym)
{
    uint8_t s0 = d->s[0], s1 = d->s[1], s2 = d->s[2], s3 = d->s[3];
    uint8_t pred, cur, e[6];
    uint32_t i, k;

    switch (d->sel) {
    case 0:  pred = 0; break;                     /* no prediction */
    case 1:  pred = s0; break;
    case 2:  pred = (uint8_t)(2 * s0 + s1); break;
    case 3:  pred = (uint8_t)(3 * (s0 + s1) - s2); break;
    case 4:  pred = (uint8_t)((int32_t)(4 * s0 - 3 * s1 + s2) >> 1); break;
    case 5:  pred = (uint8_t)(6 * (uint8_t)(42 * s2 + s1) + 4 * s0 + s3);
             break;
    default: pred = 0; break;                     /* > 5: legacy DLL predicts nothing */
    }
    cur = (uint8_t)(pred + sym);

    e[0] = cur;
    e[1] = (uint8_t)(cur - s0);
    e[2] = (uint8_t)(cur - 2 * s0 - s1);
    e[3] = (uint8_t)(cur + s2 - 3 * (s0 + s1));
    e[4] = (uint8_t)(cur - (int32_t)((int32_t)(4 * s0 - 3 * s1 + s2) >> 1));
    e[5] = (uint8_t)(cur + 4 * (uint8_t)(6 * (uint8_t)((uint8_t)(63 * s0) + s2))
                     - s1 - s3);

    if (d->count >= SQX_AUD2_8_UPDATE) {
        uint32_t cnt[6], best, idx;

        for (i = 0; i < 6; i++) {
            cnt[i] = 0;
            for (k = 0; k < 256; k++)
                if (d->freqs[i][k] > 0)
                    cnt[i]++;
        }
        best = cnt[0];
        idx = 0;
        for (i = 1; i < 6; i++)
            if (cnt[i] < best) {
                best = cnt[i];
                idx = i;
            }
        memset(d->freqs, 0, sizeof(d->freqs));
        d->sel = idx;
        d->count = 0;
    }

    for (i = 0; i < 6; i++)
        d->freqs[i][e[i]]++;
    d->count++;

    d->s[3] = s2;
    d->s[2] = s1;
    d->s[1] = s0;
    d->s[0] = cur;
    return cur;
}

/* One symbol (byte) = two nibbles (0x100226d0). */
static int aud2_8_symbol(sqx_rc *rc, sqx_bits *b, aud2_ctx8 *cs)
{
    int hi = sqx_rc_decode_symbol(rc, &cs->m[0], b);
    int lo;

    if (hi < 0)
        return -1;
    lo = sqx_rc_decode_symbol(rc, &cs->m[hi + 1], b);
    if (lo < 0)
        return -1;
    return (hi << 4) | lo;
}

static void aud2_chan8_free(aud2_chan8 *ch)
{
    free(ch->ctx);
    free(ch->freq);
    ch->ctx = NULL;
    ch->freq = NULL;
}

static int aud2_chan8_init(aud2_chan8 *ch)
{
    uint32_t c, m;

    ch->ctx = (aud2_ctx8 *)calloc(SQX_AUD2_8_CTX, sizeof(aud2_ctx8));
    ch->freq = (uint32_t *)malloc(sizeof(uint32_t) * SQX_AUD2_8_CTX *
                                  SQX_AUD2_8_SUB * SQX_AUD2_8_NSYM);
    if (!ch->ctx || !ch->freq) {
        aud2_chan8_free(ch);
        return 0;
    }
    for (c = 0; c < SQX_AUD2_8_CTX; c++) {
        for (m = 0; m < SQX_AUD2_8_SUB; m++) {
            sqx_rcmodel *mm = &ch->ctx[c].m[m];

            mm->freq = ch->freq +
                       ((size_t)c * SQX_AUD2_8_SUB + m) * SQX_AUD2_8_NSYM;
            if (!sqx_rc_model_init_ex(mm, SQX_AUD2_8_NSYM, SQX_AUD2_8_INCR,
                                       SQX_AUD2_8_MAXF, 1, SQX_AUD2_8_INCR)) {
                aud2_chan8_free(ch);
                return 0;
            }
        }
    }
    return 1;
}

static int aud_8bit_v2(sqx_bits *b, aud_out *o, uint32_t channels,
                       uint32_t raw_size)
{
    aud2_chan8 ch[2];
    aud2_delta8 *del[2] = { NULL, NULL };
    sqx_rc rc;
    uint32_t c, left = raw_size, ctx = 0;
    int ok = 0;

    memset(ch, 0, sizeof(ch));
    if (channels < 1 || channels > 2)
        return 0;
    /* Legacy DLL rejects stream if length is not a multiple of channels */
    if (raw_size % channels != 0)
        return 0;

    for (c = 0; c < channels; c++) {
        del[c] = (aud2_delta8 *)calloc(1, sizeof(aud2_delta8));
        if (!del[c] || !aud2_chan8_init(&ch[c]))
            goto out;
    }

    sqx_rc_start(&rc, b);
    while (left >= channels) {
        for (c = 0; c < channels; c++) {
            int sym = aud2_8_symbol(&rc, b, &ch[c].ctx[ctx]);

            if (sym < 0 || rc.error)
                goto out;
            ctx = (uint32_t)sym;
            aud_put8(o, aud2_delta8_decode(del[c], ctx));
        }
        left -= channels;
    }
    ok = 1;

out:
    for (c = 0; c < 2; c++) {
        if (del[c]) {
            free(del[c]);
            aud2_chan8_free(&ch[c]);
        }
    }
    return ok;
}

/* ---------------------------------------------------- 8-bit encoding (2.0) */

/* Exact mirror of decoder `aud2_delta8_decode`: returns symbol (difference)
 * for a known byte, updates the six tables and history identically (including
 * predictor re-selection at 0x240 threshold). */
static uint8_t aud2_delta8_encode(aud2_delta8 *d, uint8_t cur)
{
    uint8_t s0 = d->s[0], s1 = d->s[1], s2 = d->s[2], s3 = d->s[3];
    uint8_t pred, sym, e[6];
    uint32_t i, k;

    switch (d->sel) {
    case 0:  pred = 0; break;                     /* no prediction */
    case 1:  pred = s0; break;
    case 2:  pred = (uint8_t)(2 * s0 + s1); break;
    case 3:  pred = (uint8_t)(3 * (s0 + s1) - s2); break;
    case 4:  pred = (uint8_t)((int32_t)(4 * s0 - 3 * s1 + s2) >> 1); break;
    case 5:  pred = (uint8_t)(6 * (uint8_t)(42 * s2 + s1) + 4 * s0 + s3);
             break;
    default: pred = 0; break;                     /* > 5: legacy DLL predicts nothing */
    }
    sym = (uint8_t)(cur - pred);

    e[0] = cur;
    e[1] = (uint8_t)(cur - s0);
    e[2] = (uint8_t)(cur - 2 * s0 - s1);
    e[3] = (uint8_t)(cur + s2 - 3 * (s0 + s1));
    e[4] = (uint8_t)(cur - (int32_t)((int32_t)(4 * s0 - 3 * s1 + s2) >> 1));
    e[5] = (uint8_t)(cur + 4 * (uint8_t)(6 * (uint8_t)((uint8_t)(63 * s0) + s2))
                     - s1 - s3);

    if (d->count >= SQX_AUD2_8_UPDATE) {
        uint32_t cnt[6], best, idx;

        for (i = 0; i < 6; i++) {
            cnt[i] = 0;
            for (k = 0; k < 256; k++)
                if (d->freqs[i][k] > 0)
                    cnt[i]++;
        }
        best = cnt[0];
        idx = 0;
        for (i = 1; i < 6; i++)
            if (cnt[i] < best) {
                best = cnt[i];
                idx = i;
            }
        memset(d->freqs, 0, sizeof(d->freqs));
        d->sel = idx;
        d->count = 0;
    }

    for (i = 0; i < 6; i++)
        d->freqs[i][e[i]]++;
    d->count++;

    d->s[3] = s2;
    d->s[2] = s1;
    d->s[1] = s0;
    d->s[0] = cur;
    return sym;
}

/* Channel symbol: two nibbles (mirror of aud2_8_symbol). */
static int aud2_8_symbol_enc(sqx_rc_enc *rc, aud2_ctx8 *cs, uint8_t sym)
{
   uint32_t hi = sym >> 4, lo = sym & 0xF;

    if (!sqx_rc_enc_symbol(rc, &cs->m[0], hi))
        return 0;
    if (!sqx_rc_enc_symbol(rc, &cs->m[hi + 1], lo))
        return 0;
    return 1;
}

/* 8-bit encoder: single symbol stream (range coder mirroring decoder),
 * per-channel adaptive delta predictor, matching `aud_8bit_v2`. */
static int aud_8bit_v2_enc(sqx_bitw *w, const uint8_t *file, uint32_t skip,
                           uint32_t raw, uint32_t channels)
{
    aud2_chan8 ch[2];
    aud2_delta8 *del[2] = { NULL, NULL };
    sqx_rc_enc rc;
    uint32_t c, left = raw, ctx = 0;
    int ok = 0;

    memset(ch, 0, sizeof(ch));
    if (channels < 1 || channels > 2)
        return 0;
    if (raw % channels != 0)
        return 0;                                  /* legacy DLL also rejects misaligned size */

    for (c = 0; c < channels; c++) {
        if (!aud2_chan8_init(&ch[c]))
            goto out;
    }
    for (c = 0; c < channels; c++) {
        del[c] = (aud2_delta8 *)calloc(1, sizeof(aud2_delta8));
        if (!del[c])
            goto out;
    }

    sqx_rc_enc_init(&rc, w);
    while (left >= channels) {
        for (c = 0; c < channels; c++) {
            uint8_t cur = file[skip + (raw - left) + c];
            uint8_t sym = aud2_delta8_encode(del[c], cur);

            if (!aud2_8_symbol_enc(&rc, &ch[c].ctx[ctx], sym))
                goto out;
            ctx = (uint32_t)sym;                    /* last symbol */
        }
        left -= channels;
    }
    sqx_rc_enc_flush(&rc);
    ok = 1;

out:
    for (c = 0; c < 2; c++) {
        if (del[c])
            free(del[c]);
        aud2_chan8_free(&ch[c]);
    }
    return ok;
}

/* ------------------------------------------------------ 16-bit encoding (2.0) */

/* Method 5 encoder for SQX 2.0 format, 16-bit channels. Bitstream matches
 * `aud_16bit_v2`: 18-byte audio header, uncompressed container header bytes,
 * audio frames, and tail bytes. Each frame contains an `icd` field
 * (stereo interleaving), then per channel a packet (mode 0 polynomial or
 * mode 1 FIR) and residual body (adaptive Rice or fixed-width).
 */

typedef struct {
    uint32_t skip, raw, tail, channels, bits;
} aud_wav;

static uint32_t aud_rd16le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t aud_rd32le(const uint8_t *p)
{
    return aud_rd16le(p) | (aud_rd16le(p + 2) << 16);
}

static void aud_wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Identifies header (skip), audio data (raw), and trailing metadata (tail)
 * of a 16-bit PCM WAV. Returns 0 if unparseable. */
static int aud_wav_parse(const uint8_t *f, size_t len, aud_wav *w)
{
    size_t pos = 12, data_off = 0, data_len = 0, frame, whole;
    uint32_t fmt_tag = 0, fmt_ch = 0, fmt_bits = 0;

    if (len < 44 || memcmp(f, "RIFF", 4) != 0 || memcmp(f + 8, "WAVE", 4) != 0)
        return 0;
    while (pos + 8 <= len) {
        uint32_t csize = aud_rd32le(f + pos + 4);
        if (memcmp(f + pos, "fmt ", 4) == 0 && pos + 24 <= len && csize >= 16) {
            fmt_tag = aud_rd16le(f + pos + 8);      /* body: tag, channels, ... */
            fmt_ch = aud_rd16le(f + pos + 10);
            fmt_bits = aud_rd16le(f + pos + 22);    /* 14 bytes after tag */
        } else if (memcmp(f + pos, "data", 4) == 0) {
            data_off = pos + 8;
            data_len = csize;
            break;
        }
        pos += 8 + (size_t)csize + (csize & 1u);   /* chunks aligned to 2 bytes */
    }
    if (!data_off || fmt_tag != 1 ||
        (fmt_bits != 8 && fmt_bits != 16) ||
        (fmt_ch != 1 && fmt_ch != 2))
        return 0;
    if (data_len > len - data_off)
        data_len = len - data_off;
    /* Legacy archiver always aligns audio data to a multiple of 4,
     * placing remainder into the tail. */
    frame = data_len / 4 * 4;
    whole = frame;
    if (whole < 4)
        whole = 0;                    /* less than one frame: treat as tail */
    w->skip = (uint32_t)data_off;
    w->raw = (uint32_t)whole;
    w->tail = (uint32_t)(len - data_off - whole);
    w->channels = fmt_ch;
    w->bits = fmt_bits;
    return 1;
}

/* |v| matching DLL (INT32_MIN remains INT32_MIN) */
static uint32_t aud2_mag(int32_t v)
{
    return v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
}

/* Residuals associated with predictor (exact inverse of predictors 0..3) */
static void aud2_make_residual(uint32_t pred, const int32_t *smp,
                               uint32_t size, int32_t *res)
{
    uint32_t i;

    switch (pred) {
    case 1: {
        uint32_t prev = 0;
        for (i = 0; i < size; i++) {
            res[i] = (int32_t)((uint32_t)smp[i] - prev);
            prev = (uint32_t)smp[i];
        }
        return;
    }
    case 2: {
        uint32_t p1 = 0, p2 = 0;
        for (i = 0; i < size; i++) {
            res[i] = (int32_t)((uint32_t)smp[i] - 2u * p1 + p2);
            p2 = p1;
            p1 = (uint32_t)smp[i];
        }
        return;
    }
    case 3: {
        uint32_t p1 = 0, p2 = 0, p3 = 0;
        for (i = 0; i < size; i++) {
            res[i] = (int32_t)((uint32_t)smp[i] - 3u * (p1 - p2) - p3);
            p3 = p2;
            p2 = p1;
            p1 = (uint32_t)smp[i];
        }
        return;
    }
    default:                                   /* pred 0: copy */
        for (i = 0; i < size; i++)
            res[i] = smp[i];
        return;
    }
}

/* Sequence of `q` zero bits: SQX_PUT only handles values up to 32 bits, whereas
 * a unary quotient often exceeds 32 (up to the decoder bound). */
static void aud2_put_zero_run(sqx_bitw *w, uint32_t q)
{
    while (q > 32) {
        sqx_bitw_put(w, 0, 32);
        q -= 32;
    }
    sqx_bitw_put(w, 0, q);
}

/* Cost (in bits) and writing of the two residual bodies. */
static __attribute__((unused)) int aud2_cost_fixed(const int32_t *v, uint32_t size, uint32_t width,
                                                   uint64_t *cost)
{
    uint64_t bits = 4;                         /* width (4 bits) */

    for (uint32_t i = 0; i < size; i++) {
        uint32_t u = aud2_mag(v[i]);
        uint32_t q = width ? (u >> width) : u;
        if (q > SQX_AUD2_UNARY_MAX)
            return 0;
        bits += 2 + q + width;                 /* sign + unary + stop + value bits */
    }
    *cost = bits;
    return 1;
}

static void aud2_write_fixed(sqx_bitw *w, const int32_t *v, uint32_t size,
                             uint32_t width)
{
    for (uint32_t i = 0; i < size; i++) {
        uint32_t u = aud2_mag(v[i]);
        uint32_t q = width ? (u >> width) : u;

        sqx_bitw_put(w, v[i] < 0 ? 1u : 0u, 1);
        aud2_put_zero_run(w, q);               /* unary quotient */
        sqx_bitw_put(w, 1, 1);                 /* terminator bit */
        if (width)
            sqx_bitw_put(w, u & ((1u << width) - 1u), width);
    }
}

static int aud2_rice_step(uint32_t *k, uint32_t *accum, uint32_t u)
{
    uint32_t n = *k > 5 ? *k - 5 : 0;

    if ((u >> n) > SQX_AUD2_UNARY_MAX)
        return 0;
    *accum += u - (*accum >> 5);
    if (*k > 4 && *accum < (1u << *k))
        (*k)--;
    else if (*k < 31 && *accum >= (1u << (*k + 1)))
        (*k)++;
    return 1;
}

static uint64_t aud2_sel_cost(const int32_t *v, uint32_t size, uint32_t extra,
                              uint32_t acc0);

static int aud2_cost_rice(const int32_t *v, uint32_t size, uint64_t *cost)
{
    uint32_t k = SQX_AUD2_K_RICE, accum = 1u << SQX_AUD2_K_RICE;
    uint64_t bits = 0;

    for (uint32_t i = 0; i < size; i++) {
        uint32_t u = aud2_mag(v[i]);
        uint32_t n = k > 5 ? k - 5 : 0;

        bits += 2 + (u >> n) + n;              /* sign + unary + stop + value bits */
        if (!aud2_rice_step(&k, &accum, u))
            return 0;
    }
    *cost = bits;
    return 1;
}

static void aud2_write_rice(sqx_bitw *w, const int32_t *v, uint32_t size)
{
    uint32_t k = SQX_AUD2_K_RICE, accum = 1u << SQX_AUD2_K_RICE;

    for (uint32_t i = 0; i < size; i++) {
        uint32_t u = aud2_mag(v[i]);
        uint32_t n = k > 5 ? k - 5 : 0;

        sqx_bitw_put(w, v[i] < 0 ? 1u : 0u, 1);
        aud2_put_zero_run(w, u >> n);
        sqx_bitw_put(w, 1, 1);
        if (n)
            sqx_bitw_put(w, u & ((1u << n) - 1u), n);
        (void)aud2_rice_step(&k, &accum, u);
    }
}

typedef struct {
    uint32_t pred;
    int      flag;                             /* 1 = adaptive Rice body */
    uint32_t width;                            /* fixed-width body */
    uint64_t cost;
} aud2_enc_cfg;

/* Encoded packet configuration, mode 0 or mode 1 (FIR). Mode 1 mirrors
 * logic from sqx20u.dll 0x100219b0. */
typedef struct {
    int      mode;                             /* 0 = polynomial predictors, 1 = FIR */
    uint32_t pred;                             /* mode 0 predictor index */
    int      flag;                             /* Rice body flag (both modes) */
    uint32_t width;                            /* fixed body (mode 0) / coeff width (mode 1) */
    uint32_t body_width;                       /* fixed body width when flag = 0 (mode 1) */
    uint32_t count;                            /* mode 1: coefficient count */
    uint32_t shift;                            /* mode 1: filter shift */
    int32_t  f94;                              /* mode 1: chained A^-1 predictor flag */
    int32_t  coef[SQX_AUD2_COEF_MAX];
    uint64_t cost;                             /* total cost (bits, including body) */
    int32_t  *res;                             /* residuals for this configuration */
} aud2_enc_cfg2;

static __attribute__((unused)) uint32_t aud2_width_for(const int32_t *v, uint32_t size)
{
    uint32_t m = 0, w = 0;

    for (uint32_t i = 0; i < size; i++) {
        uint32_t u = aud2_mag(v[i]);
        if (u > m)
            m = u;
    }
    while (m) {
        w++;
        m >>= 1;
    }
    return w;
}

/* Predictor "proxy" cost: Σ|res[i]| — the DLL (fb70/fbb0/fbf0/fc50)
 * sums signed values with 32-bit wrap, `neg` leaving INT_MIN unchanged
 * (`test`/`jge`/`neg`); comparison between candidates is signed. */
static int32_t aud2_sum_abs(const int32_t *res, uint32_t size)
{
    uint32_t tot = 0;

    for (uint32_t i = 0; i < size; i++)
        tot += (uint32_t)aud2_abs32(res[i]);
    return (int32_t)tot;
}

/* Fifth estimator of mode 0 (0x1001fcc0): inverse of adaptive predictor
 * 0x1001e210 — exhaustive branches choose per sample from {v[i], d1[i], d2[i]}
 * according to a triple exponential state (ebx/ebp/esi + three 16-entry history
 * tables, updated by (base+val)>>1), and decode(res) recovers the original signal.
 * Returns Σ|out|. */
static int32_t aud2_res4_adapt(const int32_t *smp, uint32_t size, int32_t *res)
{
    int32_t a1[16], a2[16], a3[16];
    int32_t ebx = 0, ebp = 0, esi = 0;
    int32_t edi = 0, eax = 0;              /* edi = v[i-1], eax = d1[i-1] */
    uint32_t tot = 0;

    memset(a1, 0, sizeof(a1));
    memset(a2, 0, sizeof(a2));
    memset(a3, 0, sizeof(a3));
    for (uint32_t i = 0; i < size; i++) {
        uint32_t idx = i & 15u;
        int32_t edx, eax_n, m;

        /* d1[i] = v[i]-v[i-1]; d2[i] = d1[i]-d1[i-1] (32-bit wrap) */
        edx = (int32_t)((uint32_t)smp[i] - (uint32_t)edi);
        edi = smp[i];
        eax_n = (int32_t)((uint32_t)edx - (uint32_t)eax);
        eax = edx;
        res[i] = (ebx < ebp && ebx < esi) ? smp[i]
                 : (ebp < esi ? edx : eax_n);
        tot += (uint32_t)aud2_abs32(res[i]);

        m = aud2_abs32((int32_t)((uint32_t)smp[i]));
        ebx = (int32_t)((uint32_t)ebx - (uint32_t)a1[idx] + (uint32_t)m);
        a1[idx] = (int32_t)((uint32_t)a1[idx] + (uint32_t)m) >> 1;
        m = aud2_abs32(edx);
        ebp = (int32_t)((uint32_t)ebp - (uint32_t)a2[idx] + (uint32_t)m);
        a2[idx] = (int32_t)((uint32_t)a2[idx] + (uint32_t)m) >> 1;
        m = aud2_abs32(eax_n);
        esi = (int32_t)((uint32_t)esi - (uint32_t)a3[idx] + (uint32_t)m);
        a3[idx] = (int32_t)((uint32_t)a3[idx] + (uint32_t)m) >> 1;
    }
    return (int32_t)tot;
}

/* Fixed-width body cost for width w (0x1001d0c0): (w+2)·n + Σ(|v|>>w)
 * on absolute residuals buffer, with 32-bit wrap. */
static int32_t aud2_fixed_cost_w(const int32_t *abs_, uint32_t n, uint32_t w)
{
    uint32_t tot = (uint32_t)(w + 2u) * n;

    for (uint32_t i = 0; i < n; i++)
        tot += (uint32_t)((int32_t)abs_[i] >> (int)w);
    return (int32_t)tot;
}

/* Search for mode 0 fixed body width (0x1001d260): initial width w0 is the
 * smallest number of doublings of n reaching the estimated cost (capped at 15);
 * then decremented while cost strictly decreases. Returns minimum cost;
 * *w_out receives the chosen width. */
static int32_t aud2_fixed_search(const int32_t *abs_, uint32_t n,
                                 int32_t est, uint32_t *w_out)
{
    uint32_t tmp = n, w = 0;

    if ((int32_t)n < est) {
        for (;;) {
            tmp = (uint32_t)tmp << 1;      /* 32-bit wrap like x86 `shl` */
            w++;
            if (!(int32_t)tmp || (int32_t)tmp >= est)
                break;
        }
        if (w > 15u)
            w = 15u;
    }
    {
        int32_t best_c = aud2_fixed_cost_w(abs_, n, w);
        uint32_t best_w = w;

        while (w) {
            int32_t c;

            w--;
            c = aud2_fixed_cost_w(abs_, n, w);
            if (c >= best_c)
                break;
            best_c = c;
            best_w = w;
        }
        if (best_w > 15u)
            best_w = 15u;
        *w_out = best_w;
        return best_c;
    }
}

/* Best combination (predictor, body) for a channel: mirrors 0x1001fe30
 * in sqx20u.dll:
 *   - Five predictors evaluated first by Σ|res| (proxy cost, 32-bit wrap,
 *     INT_MIN preserved): polynomial predictors 0..3 (copy, 1st diff, 2nd diff,
 *     3rd diff) via aud2_make_residual, plus a 5th adaptive inverse (0x1001fcc0);
 *     winner = smallest Σ (ties broken by lowest index).
 *   - ONLY the winner receives its true body cost:
 *     fixed body via width search 0x1001d260 (doubling of n then descent);
 *     Rice body via 0x1001d3b0 (acc0 = 0); Rice selected if strictly cheaper (flag 1).
 *   - `res` receives residuals of the winning predictor. */
static uint64_t aud2_best_cfg(const int32_t *smp, uint32_t size,
                              aud2_enc_cfg *cfg, int32_t *res)
{
    uint64_t best = 0;
    int32_t *proxy[5] = { 0, 0, 0, 0, 0 };
    uint32_t pcost[5];
    uint32_t k, best_k = 0;

    if (!size)
        return 0;
    for (k = 0; k < 5; k++)
        proxy[k] = (int32_t *)malloc(sizeof(int32_t) * size);
    if (!proxy[0] || !proxy[1] || !proxy[2] || !proxy[3] || !proxy[4])
        goto fini;

    aud2_make_residual(0, smp, size, proxy[0]);
    aud2_make_residual(1, smp, size, proxy[1]);
    aud2_make_residual(2, smp, size, proxy[2]);
    aud2_make_residual(3, smp, size, proxy[3]);
    pcost[0] = (uint32_t)aud2_sum_abs(proxy[0], size);
    pcost[1] = (uint32_t)aud2_sum_abs(proxy[1], size);
    pcost[2] = (uint32_t)aud2_sum_abs(proxy[2], size);
    pcost[3] = (uint32_t)aud2_sum_abs(proxy[3], size);
    pcost[4] = (uint32_t)aud2_res4_adapt(smp, size, proxy[4]);
    for (k = 1; k < 5; k++) {
        if ((int32_t)pcost[k] < (int32_t)pcost[best_k])
            best_k = k;                        /* tie break: lower index */
    }
    memcpy(res, proxy[best_k], sizeof(int32_t) * size);
    cfg->pred = best_k;

    {
        int32_t *abs_ = proxy[0];              /* winner |res| (d230) */

        for (k = 0; k < size; k++)
            abs_[k] = aud2_abs32(res[k]);
        cfg->width = 0u;
        {
            int32_t fc = aud2_fixed_search(abs_, size,
                                           (int32_t)pcost[best_k],
                                           &cfg->width);
            /* Rice body cost is the ACTUAL body cost (0x1001d7d0, adaptive k). */
            int32_t rc;
            uint64_t rcost;

            if (aud2_cost_rice(res, size, &rcost))
                rc = (int32_t)(uint32_t)rcost;
            else
                rc = 0x7fffffff;               /* body rejected: fixed chosen */

            if (rc < fc) {
                cfg->flag = 1;
                best = (uint64_t)(uint32_t)rc;
            } else {
                cfg->flag = 0;
                best = (uint64_t)(uint32_t)fc;
            }
        }
        cfg->cost = best;
    }
fini:
    for (k = 0; k < 5; k++)
        free(proxy[k]);
    proxy[0] = NULL;
    return best;
}

/* FIR 2.0 estimator (mode 1): transcription of sqx20u.dll 0x100219b0.
 *
 * Full pipeline:
 *   0x10021870 : pre-filter B^-1: v[i] -= (3*v[i-1]) >> 2 (32-bit sar);
 *   0x10021300 : r[k] = sum v[i]*v[i-k] (k = 0..11, 64-bit accumulator);
 *   0x100213d0 : Levinson-Durbin in double (53-bit x87 precision):
 *                  J(0) = n/2 * log2((ln2)^2 * r0 / n), then for each step m:
 *                  lambda = (r[m] - sum a[j]*r[m-1-j]) / e, e -= e*(1 - lambda^2),
 *                  J(m) = n/2 * log2((ln2)^2 * e / n) + 16*m; best (min J)
 *                  fixes filter order and coefficients;
 *   0x10021590 : coefficient quantization: scale via frexp of max|a|,
 *                  q[j] = (int32)floor(a[j] * 2^shift);
 *   trials     : shift search, best Rice body selected via 0x1001d2e0 metric;
 *   0x10021910 : residuals: chained A^-1 u[i] = v[i] - (u[i-1] >> 1)
 *                  then res[i] = u[i] (i < count) and
 *                  res[i] = u[i] - (sum c[j]*u[i-1-j]) >> shift. */
/* Selector metric (0x1001d2e0 and 0x1001d3b0): bit cost of adaptive body.
 * Only difference between the two is initial accumulator value (0x4000 for FIR, 0 for mode 0).
 *   acc = acc0; ebx = 9; lo = 13 (threshold 2^14); hi = 14 (2^15);
 *   T[i] = 2^(i+1) for i = 3..30.
 * Per sample: cl = (lo < 4) ? 0 : ebx; bits += cl + (|v| >> cl);
 *   acc += |v| - sar(acc, 5) (32-bit wrap);
 *   if lo > 3: if acc < T[lo]: ebx--, lo--, hi--
 *              else if acc >= T[hi]: ebx++, lo++, hi++
 *   else (lo == 3): if acc >= T[hi]: ebx++, lo++, hi++.
 * Result = sum(bits) + 2*extra (extra = packet header in bits). */
static const uint32_t aud2_sel_tab[31] = {
    0x3fffffff, 0x7fffffff, 0xffffffff,
    0x10, 0x20, 0x40, 0x80,
    0x100, 0x200, 0x400, 0x800,
    0x1000, 0x2000, 0x4000, 0x8000,
    0x10000, 0x20000, 0x40000, 0x80000,
    0x100000, 0x200000, 0x400000, 0x800000,
    0x1000000, 0x2000000, 0x4000000, 0x8000000,
    0x10000000, 0x20000000, 0x40000000, 0x80000000
};

static int32_t aud2_s32(uint32_t x)
{
    return x & 0x80000000u ? (int32_t)(x - 0x80000000u) - 0x40000000 - 0x40000000
                           : (int32_t)x;
}

static uint64_t aud2_sel_cost(const int32_t *v, uint32_t size, uint32_t extra,
                              uint32_t acc0)
{
    int32_t acc = (int32_t)acc0;
    uint32_t ebx = 9, lo = 13, hi = 14;
    uint64_t bits = 0;

    for (uint32_t i = 0; i < size; i++) {
        uint32_t mag = (uint32_t)v[i];
        uint32_t cl, payload;

        if (v[i] < 0)
            mag = 0u - mag;
        cl = lo < 4u ? 0u : ebx;
        payload = cl ? (mag >> cl) : mag;
        bits += (uint64_t)cl + (uint64_t)payload;   /* weight + quotient */
        acc = aud2_s32((uint32_t)acc + mag - (uint32_t)aud2_s32((uint32_t)acc >> 5));
        if (lo > 3u) {
            if (acc < (int32_t)aud2_sel_tab[lo]) {
                ebx--; lo--; hi--;
            } else if ((uint32_t)acc >= aud2_sel_tab[hi]) {
                ebx++; lo++; hi++;
            }
        } else if ((uint32_t)acc >= aud2_sel_tab[hi]) {
            ebx++; lo++; hi++;
        }
    }
    return bits + 2u * (uint64_t)extra;
}

#define AUD2_FIR_LN2 0.6931471805599453

static __attribute__((unused)) uint64_t aud2_fir_bits(uint32_t count, uint32_t width, uint32_t size)
{
    (void)size;
    /* Packet header: mode(1) + count-1(5) + width(6) + count*(1+w-1)
     * + shift(4) + f94(1) + flag(1) */
    return 1u + 5u + 6u + (uint64_t)(1u + width - 1u) * count + 7u;
}

/* Arithmetic shift right: division towards -inf (x86 `sar`), 64-bit -> 32-bit. */
static int32_t aud2_sar64v(int64_t v, uint32_t s)
{
    return (int32_t)(v >> s);
}

/* x87 floor then ftol: exactly (int32_t)floor(x). */
static int32_t aud2_ftol(double x)
{
    return (int32_t)floor(x);
}

/* Mode 1 residuals for selected FIR packet:
 *   v[i]   = pre[i] - sar(sum coef[j]*pre[i-1-j], shift) for i >= count,
 *            pre[i]                                      otherwise;
 *   res[i] = v[i] - (res[i-1] >> 1)                      if f94 (chained A^-1),
 *          or v[i]                                       otherwise.
 * FIR window operates on prefiltered series `pre`, and chained A^-1
 * recursion starts from res[0] = v[0]. */
static void aud2_fir_residual(const aud2_enc_cfg2 *cfg, const int32_t *pre,
                              int32_t *u, uint32_t n, int32_t *res)
{
    (void)u;
    for (uint32_t i = 0; i < n; i++) {
        int32_t v;

        if (i < cfg->count) {
            v = pre[i];
        } else {
            int64_t acc = 0;

            for (uint32_t j = 0; j < cfg->count; j++)
                acc += (int64_t)cfg->coef[j] * pre[i - 1 - j];
            v = (int32_t)(uint32_t)((uint32_t)pre[i]
                  - (uint32_t)aud2_sar64v(acc, cfg->shift));
        }
        res[i] = (cfg->f94 && i) ?
                 (int32_t)(uint32_t)((uint32_t)v
                     - (uint32_t)(res[i - 1] >> 1)) : v;
    }
}

/* --- Pre-filter B^-1 (0x10021870): v[i] -= (3*v[i-1]) >> 2 --- */
static void aud2_prefilter(const int32_t *smp, uint32_t n, int32_t *pre)
{
    int32_t prev = 0;

    for (uint32_t i = 0; i < n; i++) {
        pre[i] = (int32_t)(uint32_t)((uint32_t)smp[i]
                  - (uint32_t)((int32_t)((int64_t)3 * prev >> 2)));
        prev = pre[i];
    }
}

static int aud2_fir_estimate(const int32_t *smp, uint32_t size,
                             aud2_enc_cfg2 *cfg, int32_t *res, int32_t *pre,
                             int32_t *u)
{
    const uint32_t n = size;
    double r[12];
    double e, best_j;
    double a[32], best_a[32], best_coef[32];
    int32_t q[32];
    uint32_t best_count = 0, best_shift = 0;
    uint32_t i, j;
    (void)u;

    memset(a, 0, sizeof(a));
    memset(best_a, 0, sizeof(best_a));

    /* -------- Pre-filter B^-1 (0x10021870) -------- */
    aud2_prefilter(smp, n, pre);

    /* --- Autocorrelations r[0..11] (0x10021300): DLL accumulates into 64-bit
     * a 32x32 product truncated to 32 bits (x86 32-bit imul), not full 64-bit product. --- */
    for (uint32_t k = 0; k <= 11u; k++) {
        int64_t s = 0;
        for (i = k; i < n; i++) {
            int64_t p = (int64_t)pre[i] * (int64_t)pre[i - k];
            int32_t pw = (int32_t)(uint32_t)((uint64_t)p & 0xffffffffu);
            s += pw;                       /* truncated 32->32 product + 64-bit sum */
        }
        r[k] = (double)s;
    }
    if (r[0] == 0.0)
        return 0;                          /* exact silence */

    /* --- Levinson-Durbin (0x100213d0, 53-bit double) --- */
    e = r[0];
    best_j = (double)n * 0.5 *
             log2(AUD2_FIR_LN2 * AUD2_FIR_LN2 * r[0] / (double)n);
    for (uint32_t m = 1; m <= 11u; m++) {
        double d = r[m], lam, jc;

        for (j = 0; j + 1 < m; j++)
            d -= a[j] * r[m - 1 - j];
        lam = d / e;
        for (j = 0; j + 1 < m; j++)
            best_a[j] = a[j] - lam * a[(m - 2) - j];
        best_a[m - 1] = lam;
        e *= 1.0 - lam * lam;
        if (e <= 0.0)
            break;
        jc = (double)n * 0.5 *
             log2(AUD2_FIR_LN2 * AUD2_FIR_LN2 * e / (double)n)
             + 16.0 * (double)m;
        if (jc < best_j || !best_count) {
            best_j = jc;
            best_count = m;
            for (j = 0; j < m; j++) {
                a[j] = best_a[j];
                best_coef[j] = best_a[j];
            }
        }
        for (j = 0; j < m; j++)
            a[j] = best_a[j];
    }
    if (best_count == 0u)
        return 0;
    for (j = 0; j < best_count; j++)
        best_a[j] = best_coef[j];          /* best order vector, not last iteration */

    /* --- Quantization (0x10021590): DLL sets scale by frexp of max|a|:
     *   e = frexp(max|a|).exp (x = m * 2^e, m in [0.5, 1))
     *   shift = 14 - e (4-bit field in packet)
     *   q[j] = floor(a[j] * 2^shift)
     *   width = bits(max|q|) + 1 (BSR + 1)
     * Adaptive Rice body is then evaluated and minimum cost is selected. */
    {
        double am = 0.0;
        int32_t ee = 0;                     /* frexp exponent: m in [0.5, 1) => e = ee */

        for (j = 0; j < best_count; j++) {
            double av = best_a[j] < 0.0 ? -best_a[j] : best_a[j];

            if (av > am)
                am = av;
        }
        while (am >= 1.0) {                 /* frexp normalization */
            am *= 0.5;
            ee++;
        }
        while (am > 0.0 && am < 0.5) {
            am *= 2.0;
            ee--;
        }
        /* ---- Trials (0x10021590 in selector loop 0x100219b0): ----
         * s0 = 14 - ee (frexp of max|a|); for k = 0..3, s = s0 + k (<= 15):
         * requantify q[j] = floor(a[j] * 2^s), width = bits(max|q|) + 1,
         * test both f94 in {0,1} variants (with/without chained A^-1);
         * body is adaptive Rice (flag = 1), measured via 0x1001d2e0 metric;
         * trial/variant with lowest total bit cost is selected. */
        {
            int found = 0;
            uint32_t best_s = 0, best_width = 0;
            int best_f94 = 0;
            int32_t best_q[32];
            uint64_t best_total = 0;

            for (uint32_t k = 0; k <= 3u; k++) {
                uint32_t s = (ee <= 14) ? (uint32_t)(14 - ee) + k : k;
                uint32_t mq2 = 0, w2, vu;
                uint64_t body, rice_body, total;
                int f94, isot = 0;

                if (s > 15u)
                    break;
                for (j = 0; j < best_count; j++) {
                    q[j] = aud2_ftol(ldexp(best_a[j], (int)s));
                    vu = (uint32_t)(q[j] < 0 ? -(uint32_t)q[j] : (uint32_t)q[j]);
                    if (vu > mq2)
                        mq2 = vu;
                }
                w2 = 0;
                while (mq2) {
                    w2++;
                    mq2 >>= 1;
                }
                w2 += 1u;
                if (w2 > 33u)
                    break;

                /* Adaptive body on residuals of each variant */
                for (f94 = 0; f94 <= (int)1; f94++) {
                    /* Residuals: v = pre (i < count) and
                     * v = pre - sar(sum q*pre, s); res = v or chained A^-1 */
                    int32_t vv = 0;

                    (void)vv;
                    for (i = 0; i < n; i++) {
                        int64_t av = 0;
                        int32_t px;

                        if (i < best_count) {
                            px = pre[i];
                        } else {
                            for (j = 0; j < best_count; j++)
                                av += (int64_t)q[j] * pre[i - 1 - j];
                            px = (int32_t)(uint32_t)
                                     ((uint32_t)pre[i]
                                      - (uint32_t)aud2_sar64v(av, s));
                        }
                        res[i] = f94 && i ?
                                 (int32_t)(uint32_t)((uint32_t)px
                                     - (uint32_t)(res[i - 1] >> 1)) : px;
                    }
                    if (!aud2_sel_cost(res, n, n, 0x4000u))
                        continue;          /* only on error */
                    body = rice_body = aud2_sel_cost(res, n, n, 0x4000u);
                    total = body + 18u + (uint64_t)w2 * best_count;
                    if (!found || total < best_total) {
                        found = 1;
                        best_total = total;
                        best_s = s;
                        best_width = w2;
                        best_f94 = f94;
                        memcpy(best_q, q, sizeof(int32_t) * best_count);
                    }
                    (void)isot;
                }
                if (ee > 14)
                    break;                 /* s0 unavailable: single trial suffices */
            }
            if (!found)
                return 0;
            best_shift = best_s;
            best_count = best_count;
            {   /* Packet fields and residuals of selected variant
                 * (recomputed exactly for emission) */
                uint32_t s = best_s;

                for (j = 0; j < best_count; j++)
                    q[j] = best_q[j];
                {   /* Selected body: recompute res with variant f94 */
                    int f94 = best_f94;

                    for (i = 0; i < n; i++) {
                        int64_t av = 0;
                        int32_t px;

                        if (i < best_count) {
                            px = pre[i];
                        } else {
                            for (j = 0; j < best_count; j++)
                                av += (int64_t)q[j] * pre[i - 1 - j];
                            px = (int32_t)(uint32_t)
                                     ((uint32_t)pre[i]
                                      - (uint32_t)aud2_sar64v(av, s));
                        }
                        res[i] = f94 && i ?
                                 (int32_t)(uint32_t)((uint32_t)px
                                     - (uint32_t)(res[i - 1] >> 1)) : px;
                    }
                }
            }
            cfg->mode = 1;
            cfg->count = best_count;
            cfg->width = best_width;
            cfg->shift = best_shift;
            cfg->f94 = best_f94;
            cfg->pred = 0;
            cfg->flag = 1;                 /* adaptive Rice body */
            cfg->body_width = 0;
            cfg->cost = best_total;
            memcpy(cfg->coef, q, sizeof(int32_t) * best_count);
            return 1;
        }
    }
}

/* Stereo interleaving: exact inverse of the 4 cases in `aud_16bit_v2`
 * (icd 0/1/2 on 16-bit values, icd 3 on 32-bit values). */
static void aud2_interleave(uint32_t icd, const int32_t *l, const int32_t *r,
                            uint32_t size, int32_t *a, int32_t *b)
{
    for (uint32_t i = 0; i < size; i++) {
        uint32_t L = (uint32_t)l[i], R = (uint32_t)r[i];

        switch (icd) {
        case 1:
            a[i] = (int32_t)L;
            b[i] = (int32_t)(int16_t)(uint16_t)(L - R);   /* Values feeding selector
                                     must be reinterpreted as signed (int16). */
            break;
        case 2:
            a[i] = (int32_t)R;
            b[i] = (int32_t)(int16_t)(uint16_t)(L - R);
            break;
        case 3: {
            /* Decoder computes c = 2a + (b&1), t = b + c, u = c - b then
             * t>>1 / u>>1: therefore a = (L + R - ((L-R)&1)) / 2 and
             * b = L - R in signed arithmetic. */
            int32_t d = (int32_t)(L - R);
            int32_t num = (int32_t)(L + R - (uint32_t)(d & 1));

            a[i] = num / 2;
            b[i] = d;
            break;
        }
        default:
            a[i] = (int32_t)L;
            b[i] = (int32_t)R;
            break;
        }
    }
}

int sqx_audio_encode(sqx_audio *a, const uint8_t *file, size_t len,
                     uint8_t **out, size_t *out_len, uint32_t *crc)
{
    aud_wav w;
    sqx_bitw bw = { 0, 0, 0, 0, 0, 0 };
    int32_t *ch[2] = { NULL, NULL };           /* Frame L/R samples */
    int32_t *ca = NULL, *cb = NULL;            /* Candidate channel sequences */
    int32_t *res[2] = { NULL, NULL };          /* Selected residuals */
    int32_t *fres = NULL, *fpre = NULL, *fu = NULL;
    aud2_enc_cfg2 cfg[2];
    uint8_t hdr[SQX_AUDIO_HDR_BYTES];
    uint32_t frames, f, c, i;
    int rc = 0;

    (void)a;
    *out = NULL;
    *out_len = 0;
    if (crc)
        *crc = 0;
    if (!aud_wav_parse(file, len, &w))
        return 0;

    /* -------- 8 bits: single symbol stream (frameless) -------- */
    if (w.bits == 8) {
        sqx_bitw b8;

        frames = w.raw / w.channels;
        sqx_bitw_init(&b8, len + 1024);
        if (b8.err)
            return 0;
        aud_wr32le(hdr + 0, w.skip);
        aud_wr32le(hdr + 4, w.tail);
        aud_wr32le(hdr + 8, w.raw);
        hdr[12] = (uint8_t)w.channels;
        hdr[13] = (uint8_t)w.bits;
        hdr[14] = hdr[15] = hdr[16] = hdr[17] = 0;
        for (i = 0; i < SQX_AUDIO_HDR_BYTES; i++)
            sqx_bitw_put(&b8, hdr[i], 8);
        for (i = 0; i < w.skip; i++)
            sqx_bitw_put(&b8, file[i], 8);
        if (!aud_8bit_v2_enc(&b8, file, w.skip, w.raw, w.channels))
            goto out8;
        for (i = 0; i < w.tail; i++)
            sqx_bitw_put(&b8, file[w.skip + w.raw + i], 8);
        sqx_bitw_flush(&b8);
        if (!b8.err) {
            if (crc) {
                uint32_t v = SQX_CRC32_INIT;
                v = sqx_crc32_raw(v, hdr, SQX_AUDIO_HDR_BYTES);
                v = sqx_crc32_raw(v, file, len);
                *crc = v;
            }
            *out = b8.buf;
            *out_len = b8.len;
            b8.buf = NULL;
        }
out8:
        sqx_bitw_free(&b8);
        return *out != NULL;
    }

    frames = w.raw / (w.channels * 2u);

    for (c = 0; c < 2; c++) {
        ch[c] = (int32_t *)malloc(sizeof(int32_t) * SQX_AUD2_FRAME_MAX);
        res[c] = (int32_t *)malloc(sizeof(int32_t) * SQX_AUD2_FRAME_MAX);
        if (!ch[c] || !res[c])
            goto out;
    }
    ca = (int32_t *)malloc(sizeof(int32_t) * SQX_AUD2_FRAME_MAX);
    cb = (int32_t *)malloc(sizeof(int32_t) * SQX_AUD2_FRAME_MAX);
    fres = (int32_t *)malloc(sizeof(int32_t) * SQX_AUD2_FRAME_MAX);
    fpre = (int32_t *)malloc(sizeof(int32_t) * SQX_AUD2_FRAME_MAX);
    fu = (int32_t *)malloc(sizeof(int32_t) * SQX_AUD2_FRAME_MAX);
    if (!ca || !cb || !fres || !fpre || !fu)
        goto out;

    sqx_bitw_init(&bw, len + 1024);
    if (bw.err)
        goto out;

    /* 18-byte audio header (same fields as read by decoder) */
    aud_wr32le(hdr + 0, w.skip);
    aud_wr32le(hdr + 4, w.tail);
    aud_wr32le(hdr + 8, w.raw);
    hdr[12] = (uint8_t)w.channels;
    hdr[13] = (uint8_t)w.bits;
    hdr[14] = hdr[15] = hdr[16] = hdr[17] = 0;
    for (i = 0; i < SQX_AUDIO_HDR_BYTES; i++)
        sqx_bitw_put(&bw, hdr[i], 8);

    /* Leading skip bytes, copied as-is */
    for (i = 0; i < w.skip; i++)
        sqx_bitw_put(&bw, file[i], 8);

    for (f = 0; f < frames; ) {
        uint32_t size = frames - f;
        uint32_t head = 0, best_icd = 0;
        uint64_t best_total = 0;

        if (size > SQX_AUD2_FRAME_MAX)
            size = SQX_AUD2_FRAME_MAX;
        else if (size < SQX_AUD2_FRAME_MAX)
            head = 1;                          /* last frame, explicit size */

        for (c = 0; c < w.channels; c++) {
            for (i = 0; i < size; i++) {
                size_t off = (size_t)w.skip +
                             ((size_t)(f + i) * w.channels + c) * 2u;
                ch[c][i] = (int16_t)(aud_rd16le(file + off));
            }
        }

        /* Stereo interleaving: select lowest cost (mono: icd 0) */
        for (uint32_t k = 0; k < (w.channels == 2 ? 4u : 1u); k++) {
            aud2_enc_cfg2 k_cfg[2];
            uint64_t cost, c2 = 0;

            memset(k_cfg, 0, sizeof(k_cfg));
            if (w.channels == 2)
                aud2_interleave(k, ch[0], ch[1], size, ca, cb);
            else {
                memcpy(ca, ch[0], sizeof(int32_t) * size);
                memset(cb, 0, sizeof(int32_t) * size);
            }
            aud2_enc_cfg m0;

            memset(&m0, 0, sizeof(m0));        /* keep call idempotent */
            cost = aud2_best_cfg(ca, size, &m0, res[0]);
            if (!cost)
                goto out;
            /* DLL compares raw body costs without the 4-bit header
             * when evaluating interleaving candidates. */
            k_cfg[0].mode = 0;
            k_cfg[0].pred = m0.pred;
            k_cfg[0].flag = m0.flag;
            k_cfg[0].width = m0.width;
            k_cfg[0].cost = m0.cost;
            if (size >= 64) {                  /* DLL limit: n < 64 rejected for FIR */
                aud2_enc_cfg2 fir;

                memset(&fir, 0, sizeof(fir));
                if (aud2_fir_estimate(ca, size, &fir, fres, fpre, fu) &&
                    fir.cost) {
                    if (fir.cost < cost) {
                        memcpy(res[0], fres, sizeof(int32_t) * size);
                        fir.res = res[0];
                        k_cfg[0] = fir;
                        cost = fir.cost;
                    }
                }
            }
            if (w.channels == 2) {
                aud2_enc_cfg m0b;

                memset(&m0b, 0, sizeof(m0b));   /* keep call idempotent */
                c2 = aud2_best_cfg(cb, size, &m0b, res[1]);
                if (!c2)
                    goto out;
                k_cfg[1].mode = 0;
                k_cfg[1].pred = m0b.pred;
                k_cfg[1].flag = m0b.flag;
                k_cfg[1].width = m0b.width;
                k_cfg[1].cost = m0b.cost;
                if (size >= 64) {
                    aud2_enc_cfg2 fir;

                    memset(&fir, 0, sizeof(fir));
                    if (aud2_fir_estimate(cb, size, &fir, fres, fpre, fu) &&
                        fir.cost && fir.cost < c2) {
                        int32_t *keep = (int32_t *)malloc(sizeof(int32_t)
                                                         * size);

                        if (keep) {
                            memcpy(keep, fres, sizeof(int32_t) * size);
                            memcpy(res[1], keep, sizeof(int32_t) * size);
                            free(keep);
                            fir.res = res[1];
                            k_cfg[1] = fir;
                            c2 = fir.cost;
                        }
                    }
                }
                cost += c2;
            }
            if (!best_total || cost <= best_total) {
                best_total = cost;
                best_icd = k;
                cfg[0] = k_cfg[0];
                cfg[1] = k_cfg[1];
            }
        }

        /* Recompute residuals for selected interleaving and configuration: */
        if (w.channels == 2)
            aud2_interleave(best_icd, ch[0], ch[1], size, ca, cb);
        else
            memcpy(ca, ch[0], sizeof(int32_t) * size);

        for (c = 0; c < w.channels; c++) {
            const int32_t *src = (c == 0) ? ca : cb;

            if (!cfg[c].mode) {
                aud2_make_residual(cfg[c].pred, src, size, res[c]);
            } else {
                aud2_prefilter(src, size, fpre);
                aud2_fir_residual(&cfg[c], fpre, fu, size, res[c]);
            }
        }

        sqx_bitw_put(&bw, head, 1);
        sqx_bitw_put(&bw, best_icd, 2);
        if (head)
            sqx_bitw_put(&bw, size, 13);
        for (c = 0; c < w.channels; c++) {
            if (cfg[c].mode == 0) {
                sqx_bitw_put(&bw, 0, 1);       /* mode 0 packet */
                sqx_bitw_put(&bw, cfg[c].pred, 3);
                sqx_bitw_put(&bw, cfg[c].flag ? 1u : 0u, 1);
                if (cfg[c].flag)
                    aud2_write_rice(&bw, res[c], size);
                else {
                    sqx_bitw_put(&bw, cfg[c].width, 4);
                    aud2_write_fixed(&bw, res[c], size, cfg[c].width);
                }
            } else {
                sqx_bitw_put(&bw, 1, 1);       /* mode 1 packet (FIR) */
                sqx_bitw_put(&bw, cfg[c].count - 1u, 5);
                sqx_bitw_put(&bw, cfg[c].width, 6);
                for (i = 0; i < cfg[c].count; i++) {
                    int32_t v = cfg[c].coef[i];

                    sqx_bitw_put(&bw, v < 0 ? 1u : 0u, 1);
                    sqx_bitw_put(&bw, (uint32_t)aud2_abs32(v),
                                 cfg[c].width - 1u);
                }
                sqx_bitw_put(&bw, cfg[c].shift, 4);
                sqx_bitw_put(&bw, cfg[c].f94 ? 1u : 0u, 1);
                sqx_bitw_put(&bw, cfg[c].flag ? 1u : 0u, 1);
                if (cfg[c].flag)
                    aud2_write_rice(&bw, res[c], size);
                else {
                    sqx_bitw_put(&bw, cfg[c].body_width, 4);
                    aud2_write_fixed(&bw, res[c], size, cfg[c].body_width);
                }
            }
        }
        f += size;
    }

    /* Trailing tail bytes, copied as-is */
    for (i = 0; i < w.tail; i++)
        sqx_bitw_put(&bw, file[w.skip + w.raw + i], 8);
    sqx_bitw_flush(&bw);
    if (bw.err)
        goto out;

    /* Audio CRC (header then full decoded file) */
    if (crc) {
        uint32_t v = SQX_CRC32_INIT;
        v = sqx_crc32_raw(v, hdr, SQX_AUDIO_HDR_BYTES);
        v = sqx_crc32_raw(v, file, len);
        *crc = v;
    }
    *out = bw.buf;
    *out_len = bw.len;
    bw.buf = NULL;
    rc = 1;

out:
    sqx_bitw_free(&bw);
    free(ca);
    free(cb);
    free(fres);
    free(fpre);
    free(fu);
    for (c = 0; c < 2; c++) {
        free(ch[c]);
        free(res[c]);
    }
    return rc;
}

/* ------------------------------------------------------------ Entry points */

int sqx_audio_alloc(sqx_audio *a)
{
    memset(a, 0, sizeof(*a));
    return 1;
}

void sqx_audio_free(sqx_audio *a)
{
    free(a->out);
    a->out = NULL;
    a->out_len = a->out_cap = 0;
}

int sqx_audio_decode(sqx_audio *a, const uint8_t *src, size_t src_len,
                     uint64_t orig_size, uint8_t arcer_ver)
{

    sqx_bits b;
    uint8_t  hdr[SQX_AUDIO_HDR_BYTES];
    uint32_t skip, tail, raw;
    uint32_t channels, bits;
    uint64_t total;
    aud_out o;

    a->unsupported = 0;
    if (src_len < SQX_AUDIO_HDR_BYTES)
        return 0;
    if (orig_size == 0 || orig_size > (uint64_t)1 << 32)
        return 0;

    sqx_bits_init(&b, src, src_len);
    for (uint32_t i = 0; i < SQX_AUDIO_HDR_BYTES; i++)
        hdr[i] = (uint8_t)sqx_bits_get(&b, 8);

    memcpy(&skip, hdr + 0, 4);
    memcpy(&tail, hdr + 4, 4);
    memcpy(&raw, hdr + 8, 4);
    channels = hdr[12];
    bits = hdr[13];

    total = (uint64_t)skip + raw + tail;
    if ((channels != 1 && channels != 2) || (bits != 8 && bits != 16))
        return 0;
    if (total != orig_size)
        return 0;

    if (a->out_cap < orig_size) {
        uint8_t *p = (uint8_t *)realloc(a->out, (size_t)orig_size);
        if (!p)
            return 0;
        a->out = p;
        a->out_cap = (size_t)orig_size;
    }
    a->out_len = 0;
    o.buf = a->out;
    o.cap = (size_t)orig_size;
    o.len = 0;

    aud_extra(&o, &b, skip);
    if (arcer_ver >= 20) {
        /* Format 2.0: sample coding from sqx20u.dll; both 8-bit and 16-bit supported. */
        if (bits != 16) {
            if (!aud_8bit_v2(&b, &o, channels, raw))
                return 0;
        } else if (!aud_16bit_v2(&b, &o, channels, raw, &a->unsupported))
            return 0;
    } else if (bits == 16) {
        if (!aud_16bit(&b, &o, channels, raw))
            return 0;
    } else {
        aud_8bit(&b, &o, channels, raw, arcer_ver);
    }
    aud_extra(&o, &b, tail);

    a->out_len = o.len;
    a->crc = SQX_CRC32_INIT;
    for (uint32_t i = 0; i < SQX_AUDIO_HDR_BYTES; i++)
        a->crc = sqx_crc32_raw(a->crc, hdr + i, 1);
    a->crc = sqx_crc32_raw(a->crc, a->out, a->out_len);
    return (uint64_t)o.len == orig_size;
}