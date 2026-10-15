/* ppm8.h — PPMd var. I ("Ppmd8") decompressor/compressor = SQX 2.0 Method 6.
 *
 * The SQX 2.0 text compression codec is PPMd var. I (Dmitry Shkarin) coupled
 * with the SQX carryless range coder (from 1.1).
 * In solid archives, the model persists across files.
 */
#ifndef SQX_PPM8_H
#define SQX_PPM8_H

#include <stddef.h>
#include <stdint.h>

typedef struct sqx_ppm8 sqx_ppm8;

sqx_ppm8 *sqx_ppm8_new(void);
void      sqx_ppm8_free(sqx_ppm8 *z);

/* Decompresses PPMd stream `src` (u32CompSize bytes) and writes `orig_size` bytes
 * into `dst` (capacity `dst_cap`). Returns 1 on success, 0 on failure. */
int sqx_ppm8_decode(sqx_ppm8 *z, const uint8_t *src, size_t src_len,
                    uint64_t orig_size, uint8_t *dst, size_t dst_cap);

/* Encodes `len` bytes into Method 6 stream and returns allocated buffer
 * via `*out`/`*out_len` (caller frees).
 *
 * `fresh` = 1: fresh model (order `order` 2..16, 0 = default 6; arena `mem_mb` MB, 0 = 32).
 * `fresh` = 0: continues existing model for solid archives.
 * Returns 1 on success. */
int sqx_ppm8_encode(sqx_ppm8 *z, const uint8_t *src, size_t len,
                    unsigned order, unsigned mem_mb, int fresh,
                    uint8_t **out, size_t *out_len);

#endif /* SQX_PPM8_H */
