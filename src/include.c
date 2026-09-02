/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The bundled BareScript include library
 *
 * Each include library script is embedded in the library as its parser-compiled JSON script model,
 * gzip-compressed (see bin/includeSource.bare). A model is decoded on first use and cached.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "barescript/includeSource.h"
#include "barescript/json.h"
#include "barescript/parser.h"
#include "barescript/runtime.h"

#include "includeSourceDecode.h"
#include "internal.h"


static BSScript *bsIncludeScripts[BS_INCLUDE_COUNT];


/*
 * Bit reader - DEFLATE packs bits LSB-first within each byte
 */

typedef struct {
    const unsigned char *data;
    size_t size;
    size_t offset;
    unsigned bitBuf;
    int bitCount;
} BsBits;


static int bsBitsNeed(BsBits *bits, int n)
{
    while (bits->bitCount < n) {
        if (bits->offset >= bits->size) {
            return -1;
        }
        bits->bitBuf |= (unsigned) bits->data[bits->offset++] << bits->bitCount;
        bits->bitCount += 8;
    }
    return 0;
}


static int bsGetBits(BsBits *bits, int n)
{
    if (n == 0) {
        return 0;
    }
    if (bsBitsNeed(bits, n) != 0) {
        return -1;
    }
    int value = (int) (bits->bitBuf & ((1u << n) - 1));
    bits->bitBuf >>= n;
    bits->bitCount -= n;
    return value;
}


/*
 * The fixed Huffman code (RFC 1951, 3.2.6) - the only one the bundled models use, since gzip.bare
 * writes a single fixed-code block. A code's bits arrive most-significant first.
 */

/* Read "n" more bits of a code onto "code"; -1 at the end of the input */
static int bsGetCodeBits(BsBits *bits, int code, int n)
{
    for (int ix = 0; ix < n; ix++) {
        int bit = bsGetBits(bits, 1);
        if (bit < 0) {
            return -1;
        }
        code = (code << 1) | bit;
    }
    return code;
}


/* Decode a literal/length symbol: 7 bits code 256-279, 8 bits 0-143 and 280-287, 9 bits 144-255 */
static int bsFixedLiteral(BsBits *bits)
{
    int code = bsGetCodeBits(bits, 0, 7);
    if (code >= 0 && code < 24) {
        return 256 + code;
    }
    code = bsGetCodeBits(bits, code, 1);
    if (code >= 0 && code < 192) {
        return code - 48;
    }
    if (code >= 0 && code < 200) {
        return 280 + code - 192;
    }
    code = bsGetCodeBits(bits, code, 1);
    return code < 0 ? -1 : 144 + code - 400;
}


static const unsigned char bsLengthExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short bsLengthBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
    131, 163, 195, 227, 258
};
static const unsigned char bsDistExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const unsigned short bsDistBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049,
    3073, 4097, 6145, 8193, 12289, 16385, 24577
};


static int bsInflateCodes(BsBits *bits, unsigned char *out, size_t outCap, size_t *outLen)
{
    for (;;) {
        int symbol = bsFixedLiteral(bits);
        if (symbol < 0) {
            return -1;
        }
        if (symbol < 256) {
            if (*outLen >= outCap) {
                return -1;
            }
            out[(*outLen)++] = (unsigned char) symbol;
            continue;
        }
        if (symbol == 256) {
            return 0;
        }
        if (symbol > 285) {
            return -1; /* GCOV_EXCL_LINE */
        }
        int extra = bsGetBits(bits, bsLengthExtra[symbol - 257]);
        if (extra < 0) {
            return -1; /* GCOV_EXCL_LINE */
        }
        unsigned length = (unsigned) (bsLengthBase[symbol - 257] + extra);
        int distSymbol = bsGetCodeBits(bits, 0, 5);
        if (distSymbol < 0 || distSymbol > 29) {
            return -1; /* GCOV_EXCL_LINE */
        }
        extra = bsGetBits(bits, bsDistExtra[distSymbol]);
        if (extra < 0) {
            return -1; /* GCOV_EXCL_LINE */
        }
        unsigned distance = (unsigned) (bsDistBase[distSymbol] + extra);
        if (distance < 1 || distance > *outLen || *outLen + length > outCap) {
            return -1; /* GCOV_EXCL_LINE */
        }
        size_t src = *outLen - distance;
        for (unsigned ix = 0; ix < length; ix++) {
            out[(*outLen)++] = out[src + ix];
        }
    }
}


static int bsInflateStored(BsBits *bits, unsigned char *out, size_t outCap, size_t *outLen)
{
    bits->bitBuf = 0;
    bits->bitCount = 0;
    int length = bsGetBits(bits, 16);
    int nlen = bsGetBits(bits, 16);
    if (length < 0 || nlen < 0 || length != (nlen ^ 65535)) {
        return -1;
    }
    if (bits->offset + (size_t) length > bits->size || *outLen + (size_t) length > outCap) {
        return -1;
    }
    memcpy(out + *outLen, bits->data + bits->offset, (size_t) length);
    bits->offset += (size_t) length;
    *outLen += (size_t) length;
    return 0;
}


static int bsInflate(BsBits *bits, unsigned char *out, size_t outCap, size_t *outLen)
{
    for (;;) {
        int final = bsGetBits(bits, 1);
        int type = bsGetBits(bits, 2);
        if (final < 0 || type < 0) {
            return -1;
        }
        int status;
        if (type == 0) {
            status = bsInflateStored(bits, out, outCap, outLen);
        } else if (type == 1) {
            status = bsInflateCodes(bits, out, outCap, outLen);
        } else {
            return -1;
        }
        if (status != 0) {
            return -1;
        }
        if (final) {
            return 0;
        }
    }
}


static uint32_t bsCrc32Table[256];
static bool bsCrc32Ready;


static void bsCrc32Init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t crc = n;
        for (int k = 0; k < 8; k++) {
            crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
        }
        bsCrc32Table[n] = crc;
    }
    bsCrc32Ready = true;
}


static uint32_t bsCrc32(const unsigned char *data, size_t size)
{
    if (!bsCrc32Ready) {
        bsCrc32Init();
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t ix = 0; ix < size; ix++) {
        crc = bsCrc32Table[(crc ^ data[ix]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}


static uint32_t bsReadU32LE(const unsigned char *data)
{
    return (uint32_t) data[0] | ((uint32_t) data[1] << 8) | ((uint32_t) data[2] << 16) |
           ((uint32_t) data[3] << 24);
}


/* Advance past a NUL-terminated gzip extra field (filename or comment) */
static size_t bsGzipSkipString(const unsigned char *src, size_t srcSize, size_t offset)
{
    while (offset < srcSize && src[offset] != 0) {
        offset++;
    }
    return offset + 1;
}


char *bsGzipUncompress(const unsigned char *src, size_t srcSize)
{
    if (srcSize < 10) {
        return NULL;
    }
    if (src[0] != 0x1f || src[1] != 0x8b || src[2] != 8) {
        return NULL;
    }
    unsigned flags = src[3];
    if ((flags & 0xE0u) != 0) {
        return NULL;
    }

    size_t offset = 10;
    if ((flags & 4u) != 0) {
        if (offset + 2 > srcSize) {
            return NULL;
        }
        unsigned xlen = (unsigned) src[offset] | ((unsigned) src[offset + 1] << 8);
        offset += 2 + xlen;
    }
    if ((flags & 8u) != 0) {
        offset = bsGzipSkipString(src, srcSize, offset);
    }
    if ((flags & 16u) != 0) {
        offset = bsGzipSkipString(src, srcSize, offset);
    }
    if ((flags & 2u) != 0) {
        offset += 2;
    }
    if (offset + 8 > srcSize) {
        return NULL;
    }

    uint32_t crcExpected = bsReadU32LE(src + srcSize - 8);
    uint32_t isize = bsReadU32LE(src + srcSize - 4);
    unsigned char *out = bsAlloc((size_t) isize + 1);
    size_t outLen = 0;
    BsBits bits;
    memset(&bits, 0, sizeof(bits));
    bits.data = src + offset;
    bits.size = srcSize - 8 - offset;
    if (bsInflate(&bits, out, isize, &outLen) != 0 || outLen != isize ||
        bsCrc32(out, outLen) != crcExpected) {
        free(out);
        return NULL;
    }
    out[outLen] = '\0';
    return (char *) out;
}


const char *bsIncludeSourceDecode(BSIncludeSource *source)
{
    if (source->decoded != NULL) {
        return source->decoded;
    }
    if (source->gzip == NULL || source->gzipSize == 0) {
        return NULL;
    }
    source->decoded = bsGzipUncompress(source->gzip, source->gzipSize);
    return source->decoded;
}


size_t bsIncludeCount(void)
{
    return BS_INCLUDE_COUNT;
}


const char *bsIncludeName(size_t index)
{
    return index < BS_INCLUDE_COUNT ? bsIncludeSources[index].name : NULL;
}


/* A bundled include's registry index, or BS_INCLUDE_COUNT if there is no such include */
static size_t bsIncludeFind(const char *name)
{
    size_t ix = 0;
    while (ix < BS_INCLUDE_COUNT && strcmp(bsIncludeSources[ix].name, name) != 0) {
        ix++;
    }
    return ix;
}


const char *bsIncludeSource(const char *name)
{
    size_t ix = bsIncludeFind(name);
    return ix < BS_INCLUDE_COUNT ? bsIncludeSourceDecode(&bsIncludeSources[ix]) : NULL;
}


BSScript *bsIncludeScript(const char *name)
{
    size_t ix = bsIncludeFind(name);
    if (ix == BS_INCLUDE_COUNT) {
        return NULL;
    }
    if (bsIncludeScripts[ix] != NULL) {
        return bsScriptRetain(bsIncludeScripts[ix]);
    }
    const char *text = bsIncludeSourceDecode(&bsIncludeSources[ix]);
    if (text == NULL) {
        return NULL; /* GCOV_EXCL_LINE - bundled models always inflate */
    }
    BSValue model = bsJSONDecode(text, strlen(text), NULL);
    BSScript *script = bsScriptFromModel(model, name);
    bsRelease(model);
    free(bsIncludeSources[ix].decoded);
    bsIncludeSources[ix].decoded = NULL;
    if (script == NULL) {
        return NULL; /* GCOV_EXCL_LINE - bundled models always convert */
    }
    script->system = true;
    bsScriptDropModel(script);
    bsIncludeScripts[ix] = bsScriptRetain(script);
    return script;
}


void bsIncludeCleanup(void)
{
    for (size_t ix = 0; ix < BS_INCLUDE_COUNT; ix++) {
        free(bsIncludeSources[ix].decoded);
        bsIncludeSources[ix].decoded = NULL;
        if (bsIncludeScripts[ix] != NULL) {
            bsScriptRelease(bsIncludeScripts[ix]);
            bsIncludeScripts[ix] = NULL;
        }
    }
}
