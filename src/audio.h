/* audio.h — SQX Audio codec (Method 5), port of cx_audio.c from SQX SDK 2.06
 * and reverse-engineered SQX 2.0 16-bit/8-bit audio codecs.
 *
 * Stream starts with an 18-byte header read by the bit reader (SQX_AUDIO_HEAD):
 *   lSkipSize    4 bytes  Header size (44 for standard WAV header)
 *   lFileTail    4 bytes  Raw uncompressed bytes at end of file
 *   uRawDataSize 4 bytes  Encoded audio payload bytes
 *   cChannels    1 byte   1 (mono) or 2 (stereo)
 *   cChannelBits 1 byte   8 or 16 bits per sample
 *   2 bytes padding, then 2 bytes extra
 *
 * Codecs:
 *   1.1: 8-bit channels use Huffman delta coding; 16-bit channels use
 *        384-sample frames with polynomial prediction.
 *   2.0: 16-bit channels use 4608-sample frames, adaptive FIR / predictor modes,
 *        Rice entropy coding, and channel interleaving (`icd`).
 *        8-bit channels use range coder + 256 nibble contexts + adaptive delta.
 */
#ifndef SQX_AUDIO_H
#define SQX_AUDIO_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *out;
    size_t   out_len, out_cap;
    /* SDK calculates archive file CRC over the 18-byte audio header
     * concatenated with the entire plaintext file. */
    uint32_t crc;
    int      unsupported;
} sqx_audio;

int  sqx_audio_alloc(sqx_audio *a);
void sqx_audio_free(sqx_audio *a);

/* Decompresses audio stream `src` (src_len bytes) and produces orig_size bytes.
 * Returns 1 on success, 0 on error. */
int sqx_audio_decode(sqx_audio *a, const uint8_t *src, size_t src_len,
                     uint64_t orig_size, uint8_t arcer_ver);

/* Encodes 16-bit PCM WAV `file` into SQX 2.0 Method 5 bitstream.
 * Allocated buffer returned in `*out`/`*out_len` (caller frees).
 * Returns 1 on success, 0 on failure. */
int sqx_audio_encode(sqx_audio *a, const uint8_t *file, size_t len,
                     uint8_t **out, size_t *out_len, uint32_t *crc);

#endif /* SQX_AUDIO_H */
