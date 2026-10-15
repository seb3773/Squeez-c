/* delta.h — SQX delta transforms (port of t_coder.c).
 *
 * Two use cases:
 *   - DeconvRType: delta applied to multimedia range-coded blocks (parameters
 *     encoded in the MM block bit-header);
 *   - DeconvR_LZ_Type: delta applied to the output of LZ (parameters in the
 *     "delta header" following usExtCompressor, cf. cCompFlags bit 0).
 */
#ifndef SQX_DELTA_H
#define SQX_DELTA_H

#include <stdint.h>

/* Initial adaptation period for delta models (0xFF in both formats) */
#define SQX_STD_DELTA_FREQ 0xFF
#define SQX_DCODER_8BIT    0
#define SQX_DCODER_16BIT   1

typedef struct {
    uint32_t freqs[6][256];
    uint8_t  samps[8];
    uint32_t delta;
    uint32_t update_freq;
    uint32_t curr_count;
} sqx_delta;

typedef struct {
    uint32_t freqs_high[4][256];
    uint32_t freqs_low[4][256];
    uint16_t samps[8];
    uint32_t delta;
    uint32_t update_freq;
    uint32_t curr_count;
} sqx_delta16;

void     sqx_delta_init(sqx_delta *d, uint32_t start_delta, uint32_t update_freq);
void     sqx_delta16_init(sqx_delta16 *d, uint32_t start_delta, uint32_t update_freq);
uint8_t  sqx_delta_decode8(sqx_delta *d, uint8_t c);
uint16_t sqx_delta_decode16(sqx_delta16 *d, uint16_t w);

void sqx_deconv_rx(sqx_delta *d, uint32_t num_coder, uint8_t *buf, uint32_t size);
void sqx_deconv_r4_16(sqx_delta16 *d, uint8_t *buf, uint32_t size);

/* Standard DeconvRType delta: coder_type = 8 or 16 bits */
void sqx_deconv_rtype(uint32_t start_delta, uint32_t coder_type,
                      uint32_t coder_num, uint32_t update_freq,
                      uint8_t *buf, uint32_t size);

#endif /* SQX_DELTA_H */
