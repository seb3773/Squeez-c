/* rc.h — SQX carryless range coder (Mikael Lundqvist algorithm).
 * Faithful port of rcoder.c from SQX SDK. Used by multimedia (MM) blocks
 * and delta transforms.
 */
#ifndef SQX_RC_H
#define SQX_RC_H

#include "bitio.h"
#include "bitw.h"
#include <stdint.h>

#define SQX_RC_TOP  (1u << 24)
#define SQX_RC_BOT  (1u << 16)

typedef struct {
    uint32_t low, range, code, passed, error;
} sqx_rc;

typedef struct {
    uint32_t *freq;
    uint32_t tot_freq;
    uint32_t incr;
    uint32_t max_freq;
    uint32_t adapt;
    uint32_t half_freq;
    uint32_t first_freq;
    uint32_t last_freq;
    uint32_t last_sym;
    uint32_t last_cum_freq;
    uint32_t nsym, nsym2, nsym3, nsym4, nsym23;
} sqx_rcmodel;

int  sqx_rc_model_create(sqx_rcmodel *m, uint32_t nsym, uint32_t incr,
                         uint32_t max_freq, uint32_t adapt);
int  sqx_rc_model_create_ex(sqx_rcmodel *m, uint32_t nsym, uint32_t incr,
                            uint32_t max_freq, uint32_t adapt,
                            uint32_t init_freq);
int  sqx_rc_model_init(sqx_rcmodel *m, uint32_t nsym, uint32_t incr,
                       uint32_t max_freq, uint32_t adapt);
int  sqx_rc_model_init_ex(sqx_rcmodel *m, uint32_t nsym, uint32_t incr,
                          uint32_t max_freq, uint32_t adapt,
                          uint32_t init_freq);
void sqx_rc_model_free(sqx_rcmodel *m);

typedef struct {
    uint32_t low, range;
    uint32_t window;              /* Decoder "code" window (4 bytes) */
    uint32_t written;             /* Bytes written so far */
    sqx_bitw *w;                  /* Associated bit writer */
} sqx_rc_enc;

void sqx_rc_start(sqx_rc *rc, sqx_bits *b);
int  sqx_rc_decode_symbol(sqx_rc *rc, sqx_rcmodel *m, sqx_bits *b);

/* Encoder interface: exact mirror of decoder */
int  sqx_rc_enc_init(sqx_rc_enc *e, sqx_bitw *w);
int  sqx_rc_enc_symbol(sqx_rc_enc *e, sqx_rcmodel *m, unsigned sym);
void sqx_rc_enc_flush(sqx_rc_enc *e);

#endif /* SQX_RC_H */
