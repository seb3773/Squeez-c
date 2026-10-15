/* lz.h — SQX LZ77 + Huffman decompressor v1.1 and v2.0 (Methods 1-4).
 * Port of uarde.c/ctab.c/arcio.c/mm_coder.c from SQX SDK 2.06.
 */
#ifndef SQX_LZ_H
#define SQX_LZ_H

#include "bitio.h"
#include "delta.h"
#include "huff.h"
#include "ir.h"
#include "rc.h"
#include <stdint.h>

#define SQX_MIN_LEN2     0x000000FFu
#define SQX_MIN_LEN3     0x00003FFFu
#define SQX_MIN_LEN4     0x0003FFFFu
#define SQX_MAX_DIC_SIZE 0x400000u      /* 4 MB default SDK buffer size */

/* Filter flags (usExtCompressor) */
#define SQX_IA32IMAGE_TYPE_FLAG 0x0002
#define SQX_DELTA_TYPE_FLAG     0x0004

typedef struct {
    /* Persistent state across files in solid mode */
    uint8_t  *window;            /* Sliding dictionary window */
    uint32_t  dic_size, mask;
    uint32_t  curr_pos, write_pos;
    uint32_t  last_len, last_dist, prev_dists[4], prev_idx;
    uint32_t  dist_state;        /* 2.0: current distance (symbol 257) */
    uint64_t  to_flush;

    /* Huffman tables (reloaded per block) */
    sqx_huff  bitct, litlen[2], rep, dist;
    sqx_huff  low16;             /* 2.0: lower 4-bit distance table */
    int       have16;            /* 2.0: low16 table present in block */
    uint8_t   low16_width;       /* 2.0: bit width for low16 lengths */
    sqx_dist_tab dtabs;

    /* Multimedia range-coded blocks */
    sqx_rc       rc;
    sqx_rcmodel  mm[4];

    /* Transforms and filters (per-file settings) */
    uint16_t  ext_compressor;
    int       ext_present;
    uint32_t  dlt_type, dlt_num;
    int32_t   dlt_offset, dlt_off_so_far;
    int       dlt_need_ofs, dlt_code_deltas, lz_delta_inited;
    sqx_delta   lz_delta[4];
    sqx_delta16 lz_delta16;
    int       img_flag;
    int32_t   image_size, image_last_count;

    int       fmt20;             /* Format 2.0: bit-header variants */
    uint32_t  blk_flag;          /* 2.0: extra flag in LZ block header */
    sqx_ir   *ir;                /* Non-null: logs decoded operations into IR */

    /* Output buffer */
    uint8_t  *imgbuf;
    uint8_t  *out;
    size_t    out_len, out_cap;
    uint32_t  file_crc;          /* Raw CRC32 of decompressed plaintext */
} sqx_lz;

int  sqx_lz_alloc(sqx_lz *z);
void sqx_lz_free(sqx_lz *z);

/* Decompresses stream `src` (u32CompSize bytes) and generates `orig_size` bytes.
 * `solid` = bit 2 of file header flags (continuation of solid archive).
 * `comp_flags` = cCompFlags from file header (bit 0 => usExtCompressor present).
 * Returns 1 on success, 0 on error. */
int sqx_lz_decode(sqx_lz *z, const uint8_t *src, size_t src_len,
                  uint64_t orig_size, int solid, uint32_t dic_size,
                  uint8_t comp_flags, uint8_t arcer_ver);

/* Decompresses stream and captures Intermediate Representation (IR) into `ir` */
int sqx_lz_decode_ir(sqx_lz *z, const uint8_t *src, size_t src_len,
                     uint64_t orig_size, int solid, uint32_t dic_size,
                     uint8_t comp_flags, uint8_t arcer_ver, sqx_ir *ir);

#endif /* SQX_LZ_H */
