/* sqx.h — SQX (Squeez) archive container format v1.1 / v2.0
 *
 * Reverse-engineered specification for interoperability.
 * ANSI C99, zero external dependencies.
 */
#ifndef SQX_H
#define SQX_H

#include <stddef.h>
#include <stdint.h>

/* Block types */
#define SQX_BLOCK_MAIN      0x52
#define SQX_BLOCK_FILE      0x44
#define SQX_BLOCK_DIR       0x56
#define SQX_BLOCK_END       0x53
#define SQX_BLOCK_COMMENT   0x43
#define SQX_BLOCK_RECOVERY  0x58
#define SQX_BLOCK_AUTH      0x41
#define SQX_BLOCK_INTERNAL  0x49

#define SQX_BLOCK_HDR_SIZE  7
#define SQX_MAX_BLOCK_SIZE  0x8000   /* Extended headers (> 2 KB) for delta/audio/etc. */

/* Compression methods (SQX SDK definitions) */
enum {
    SQX_METHOD_STORED = 0,
    SQX_METHOD_NORMAL = 1,   /* LZH v1 */
    SQX_METHOD_GOOD   = 2,   /* LZH v1 */
    SQX_METHOD_HIGH   = 3,   /* LZH v1 */
    SQX_METHOD_BEST   = 4,   /* LZH v1 */
    SQX_METHOD_AUDIO  = 5,
    SQX_METHOD_TEXT   = 6,   /* PPMd (SQX2) */
    SQX_METHOD_ULTRA  = 7    /* LZ77 + RC (SQX2) */
};

/* Archive format version (cArcerExVer) */
#define SQX_ARCVER_11   11   /* 0x0b */
#define SQX_ARCVER_20   20   /* 0x14 */

typedef struct {
    size_t   offset;        /* Block byte offset in archive */
    uint16_t crc16;         /* Stored CRC16 (lower 16 bits of un-inverted CRC32) */
    uint8_t  type;
    uint16_t flags;
    uint16_t size;          /* Total block size including header (>= 7) */
    const uint8_t *raw;     /* Pointer to raw block data (size bytes) */
} sqx_block;

typedef struct {
    uint8_t  arcer_ver;     /* 11 = 1.1, 20 = 2.0 */
    uint8_t  os_filesys;
    uint8_t  method;
    uint8_t  comp_flags;
    uint16_t extra_flags;
    uint32_t crc32;         /* CRC32 of original plaintext file */
    uint32_t attrs;
    uint32_t time;          /* MS-DOS bit-packed timestamp */
    uint64_t comp_size;
    uint64_t orig_size;
    uint16_t name_len;
    const char *name;       /* Not NUL-terminated (name_len bytes) */
    size_t   data_offset;   /* Byte offset of compressed payload */
    size_t   next_offset;   /* Byte offset of next block (after payload) */
} sqx_file;

typedef struct {
    int      file_format;   /* 11 or 20; 0 = unknown */
    uint16_t flags;
    uint32_t vol_id;
    uint16_t vol_num;
} sqx_main;

/* --- Public container primitives --- */

/* Raw CRC32 matching SQX: reflected polynomial 0xEDB88320,
 * init 0xFFFFFFFF, WITHOUT final XOR. */
uint32_t sqx_crc32_raw(uint32_t crc, const void *buf, size_t len);
#define SQX_CRC32_INIT 0xFFFFFFFFu

/* Block CRC16 = low16(sqx_crc32_raw(INIT, buf, len)) */
uint16_t sqx_crc16(const void *buf, size_t len);

/* Validates that the block at raw[0..] is consistent; returns 1 on success.
 * avail = number of available bytes starting from buf. */
int sqx_block_read(const uint8_t *buf, size_t avail, sqx_block *out);
int sqx_block_check_crc(const sqx_block *b);

int sqx_parse_main(const sqx_block *b, sqx_main *out);
int sqx_parse_file(const sqx_block *b, const uint8_t *buf, size_t avail, sqx_file *out);

const char *sqx_block_name(uint8_t type);
const char *sqx_method_name(uint8_t method, uint8_t arcer_ver);
uint64_t sqx_dict_size(uint16_t flags, uint8_t arcer_ver);
const char *sqx_dict_name(uint16_t flags, uint8_t arcer_ver);

#endif /* SQX_H */
