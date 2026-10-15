/* create.h — SQX encoder (compression): input file(s) -> .sqx archive. */
#ifndef SQX_CREATE_H
#define SQX_CREATE_H

#include <stddef.h>
#include <stdint.h>

/* Creates an archive containing `n_in` files, using method `method` (1..4 =
 * LZ/Huffman, 5 = audio 2.0 from 16-bit PCM WAV, 6 = PPMd — the latter two
 * available in SQX 2.0 only), dictionary `dic_size` bytes, format
 * `arcer_ver` (11 or 20). `ppm_order` / `ppm_mem_mb` apply to method 6 only
 * (0 = defaults: order 6, 32 MB, matching the legacy oracle).
 * Returns 0 on success. */
int sqx_create_archive(const char *out_path, char *const *in_paths, int n_in,
                       int arcer_ver, int method, uint32_t dic_size,
                       unsigned ppm_order, unsigned ppm_mem_mb);

/* Compresses a memory buffer: produces the raw LZ/Huffman stream (without container). */
int sqx_create_stream(const uint8_t *buf, size_t len, int arcer_ver,
                      uint32_t dic_size, int effort, uint8_t **out,
                      size_t *out_len);

#endif /* SQX_CREATE_H */
