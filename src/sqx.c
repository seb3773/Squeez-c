/* sqx.c — Implementation of the SQX container format */
#include "sqx.h"
#include <stdio.h>
#include <string.h>

uint32_t sqx_crc32_raw(uint32_t crc, const void *buf, size_t len)
{
    static uint32_t table[256];
    static int init = 0;
    const uint8_t *p = (const uint8_t *)buf;

    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = 1;
    }
    for (size_t i = 0; i < len; i++)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

uint16_t sqx_crc16(const void *buf, size_t len)
{
    return (uint16_t)(sqx_crc32_raw(SQX_CRC32_INIT, buf, len) & 0xFFFF);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int sqx_block_read(const uint8_t *buf, size_t avail, sqx_block *out)
{
    if (avail < SQX_BLOCK_HDR_SIZE)
        return 0;
    out->crc16  = rd16(buf);
    out->type   = buf[2];
    out->flags  = rd16(buf + 3);
    out->size   = rd16(buf + 5);
    if (out->size < SQX_BLOCK_HDR_SIZE || (size_t)out->size > avail)
        return 0;
    out->raw = buf;
    return 1;
}

int sqx_block_check_crc(const sqx_block *b)
{
    return sqx_crc16(b->raw + 2, (size_t)b->size - 2) == b->crc16;
}

int sqx_parse_main(const sqx_block *b, sqx_main *out)
{
    const uint8_t *p = b->raw + SQX_BLOCK_HDR_SIZE;
    size_t plen = (size_t)b->size - SQX_BLOCK_HDR_SIZE;

    if (b->type != SQX_BLOCK_MAIN || plen < 18)
        return 0;
    if (memcmp(p, "-sqx-", 5) != 0)
        return 0;

    /* The version byte in the main header is always 11 (even in 2.0 archives):
     * the actual format version is read per file header (cArcerExVer). */
    out->file_format = 0;
    out->flags = b->flags;
    out->vol_num = rd16(p + 6);
    out->vol_id  = rd32(p + 8);
    return 1;
}

int sqx_parse_file(const sqx_block *b, const uint8_t *buf, size_t avail, sqx_file *out)
{
    size_t off = b->offset + SQX_BLOCK_HDR_SIZE;

    if (b->type != SQX_BLOCK_FILE)
        return 0;
    if (avail < off + 26)
        return 0;

    out->comp_flags  = buf[off];
    out->extra_flags = rd16(buf + off + 1);
    out->os_filesys  = buf[off + 3];
    out->arcer_ver   = buf[off + 4];
    out->method      = buf[off + 5];
    out->crc32       = rd32(buf + off + 6);
    out->attrs       = rd32(buf + off + 10);
    out->time        = rd32(buf + off + 14);
    out->comp_size   = rd32(buf + off + 18);
    out->orig_size   = rd32(buf + off + 22);
    off += 26;

    if (b->flags & 0x0080) {           /* bit 7: 64-bit sizes */
        if (avail < off + 8)
            return 0;
        out->comp_size |= (uint64_t)rd32(buf + off) << 32;
        out->orig_size |= (uint64_t)rd32(buf + off + 4) << 32;
        off += 8;
    }
    if (avail < off + 2)
        return 0;
    out->name_len = rd16(buf + off);
    off += 2;
    if (avail < off + out->name_len)
        return 0;
    out->name = (const char *)(buf + off);
    off += out->name_len;

    out->data_offset = off;                       /* Payload data = comp_size bytes */
    out->next_offset = off + (size_t)out->comp_size;
    if (out->next_offset > avail)                 /* Truncated archive safety guard */
        out->next_offset = avail;
    return 1;
}

const char *sqx_block_name(uint8_t type)
{
    switch (type) {
    case SQX_BLOCK_MAIN:     return "MAIN";
    case SQX_BLOCK_FILE:     return "FILE";
    case SQX_BLOCK_DIR:      return "DIR";
    case SQX_BLOCK_END:      return "END";
    case SQX_BLOCK_COMMENT:  return "COMMENT";
    case SQX_BLOCK_RECOVERY: return "RECOVERY";
    case SQX_BLOCK_AUTH:     return "AUTH";
    case SQX_BLOCK_INTERNAL: return "INTERNAL";
    default:                 return "?";
    }
}

const char *sqx_method_name(uint8_t method, uint8_t arcer_ver)
{
    (void)arcer_ver;
    switch (method) {
    case SQX_METHOD_STORED: return "stored";
    case SQX_METHOD_NORMAL: return "normal(LZH)";
    case SQX_METHOD_GOOD:   return "good(LZH)";
    case SQX_METHOD_HIGH:   return "high(LZH)";
    case SQX_METHOD_BEST:   return "best(LZH)";
    case SQX_METHOD_AUDIO:  return "audio";
    case SQX_METHOD_TEXT:   return "text(PPMd)";
    case SQX_METHOD_ULTRA:  return "ultra(LZ77+RC)";
    default:                return "?";
    }
}

uint64_t sqx_dict_size(uint16_t flags, uint8_t arcer_ver)
{
    unsigned idx = (flags >> 8) & 7;
    if (arcer_ver >= SQX_ARCVER_20 && (flags & 0x0800))
        return (uint64_t)8 << (20 + idx);          /* 8 MB << idx */
    return (uint64_t)1 << (15 + idx);              /* 32 KB << idx */
}

const char *sqx_dict_name(uint16_t flags, uint8_t arcer_ver)
{
    static char buf[16];
    uint64_t sz = sqx_dict_size(flags, arcer_ver);
    if (sz >= 1024 * 1024)
        snprintf(buf, sizeof(buf), "%lluM", (unsigned long long)(sz >> 20));
    else
        snprintf(buf, sizeof(buf), "%lluK", (unsigned long long)(sz >> 10));
    return buf;
}
