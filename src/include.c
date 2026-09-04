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


/* The thread's compiled include scripts and decoded model texts - the registry itself is never written */
static _Thread_local BSScript *bsIncludeScripts[BS_INCLUDE_COUNT];
static _Thread_local char *bsIncludeDecoded[BS_INCLUDE_COUNT];


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


/* Fill the bit buffer to "n" bits where the input allows; bitCount is how many it then holds */
static void bsBitsFill(BsBits *bits, int n)
{
    while (bits->bitCount < n && bits->offset < bits->size) {
        bits->bitBuf |= (unsigned) bits->data[bits->offset++] << bits->bitCount;
        bits->bitCount += 8;
    }
}


static int bsGetBits(BsBits *bits, int n)
{
    bsBitsFill(bits, n);
    if (bits->bitCount < n) {
        return -1;
    }
    int value = (int) (bits->bitBuf & ((1u << n) - 1));
    bits->bitBuf >>= n;
    bits->bitCount -= n;
    return value;
}


/*
 * The fixed Huffman code (RFC 1951, 3.2.6) - the only one the bundled models use, since gzip.bare
 * writes a single fixed-code block. A code's bits arrive most-significant first, so the decode
 * table is indexed by the next nine stream bits in stream order and holds each code's length and
 * symbol; the code is complete, so every index has an entry.
 */
#define BS_FIXED_TABLE_BITS 9
#define BS_FIXED_TABLE_SIZE (1u << BS_FIXED_TABLE_BITS)

/* Build the literal/length table: 7 bits code 256-279, 8 bits 0-143 and 280-287, 9 bits 144-255 */
static void bsFixedTable(uint16_t *table)
{
    for (unsigned symbol = 0; symbol < 288; symbol++) {
        unsigned code;
        unsigned length;
        if (symbol < 144) {
            code = 0x30 + symbol;
            length = 8;
        } else if (symbol < 256) {
            code = 0x190 + symbol - 144;
            length = 9;
        } else if (symbol < 280) {
            code = symbol - 256;
            length = 7;
        } else {
            code = 0xC0 + symbol - 280;
            length = 8;
        }
        unsigned reversed = 0;
        for (unsigned ix = 0; ix < length; ix++) {
            reversed |= ((code >> ix) & 1u) << (length - 1 - ix);
        }
        for (unsigned index = reversed; index < BS_FIXED_TABLE_SIZE; index += 1u << length) {
            table[index] = (uint16_t) ((length << BS_FIXED_TABLE_BITS) | symbol);
        }
    }
}


/* Decode a literal/length symbol; -1 at the end of the input */
static int bsFixedLiteral(BsBits *bits, const uint16_t *table)
{
    bsBitsFill(bits, BS_FIXED_TABLE_BITS);
    unsigned entry = table[bits->bitBuf & (BS_FIXED_TABLE_SIZE - 1)];
    int length = (int) (entry >> BS_FIXED_TABLE_BITS);
    if (length > bits->bitCount) {
        return -1;
    }
    bits->bitBuf >>= length;
    bits->bitCount -= length;
    return (int) (entry & (BS_FIXED_TABLE_SIZE - 1));
}


/* Decode a distance symbol - a five-bit code, most-significant bit first; -1 at the end of the input */
static int bsFixedDistance(BsBits *bits)
{
    int value = bsGetBits(bits, 5);
    if (value < 0) {
        return -1;
    }
    return ((value & 1) << 4) | ((value & 2) << 2) | (value & 4) | ((value & 8) >> 2) | ((value & 16) >> 4);
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
    uint16_t table[BS_FIXED_TABLE_SIZE];
    bsFixedTable(table);
    for (;;) {
        int symbol = bsFixedLiteral(bits, table);
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
        int distSymbol = bsFixedDistance(bits);
        if (distSymbol < 0 || distSymbol > 29) {
            return -1; /* GCOV_EXCL_LINE */
        }
        extra = bsGetBits(bits, bsDistExtra[distSymbol]);
        if (extra < 0) {
            return -1; /* GCOV_EXCL_LINE */
        }
        unsigned distance = (unsigned) (bsDistBase[distSymbol] + extra);
        if (distance > *outLen || *outLen + length > outCap) {
            return -1; /* GCOV_EXCL_LINE */
        }
        size_t src = *outLen - distance;
        if (distance >= length) {
            memcpy(out + *outLen, out + src, length);
            *outLen += length;
        } else {
            /* The match overlaps its own output - it repeats the last "distance" bytes */
            for (unsigned ix = 0; ix < length; ix++) {
                out[(*outLen)++] = out[src + ix];
            }
        }
    }
}


static uint32_t bsReadU32LE(const unsigned char *data)
{
    return (uint32_t) data[0] | ((uint32_t) data[1] << 8) | ((uint32_t) data[2] << 16) |
           ((uint32_t) data[3] << 24);
}


/*
 * The bundled models are written by gzip.bare's compressor (see bin/includeSource.bare), whose
 * output has one shape: the ten-byte header with no optional fields, a single final block of fixed
 * Huffman codes, and the CRC and size trailer. Only that shape is decoded. The data is compiled in,
 * so the inflated size is the one integrity check it needs; the CRC is not verified.
 */
char *bsGzipUncompress(const unsigned char *src, size_t srcSize)
{
    if (srcSize < 18 || src[0] != 0x1f || src[1] != 0x8b || src[2] != 8 || src[3] != 0) {
        return NULL;
    }
    uint32_t isize = bsReadU32LE(src + srcSize - 4);
    unsigned char *out = bsAlloc((size_t) isize + 1);
    size_t outLen = 0;
    BsBits bits = {src + 10, srcSize - 18, 0, 0, 0};
    if (bsGetBits(&bits, 3) != 3 || bsInflateCodes(&bits, out, isize, &outLen) != 0 || outLen != isize) {
        free(out);
        return NULL;
    }
    out[outLen] = '\0';
    return (char *) out;
}


const char *bsIncludeSourceDecode(size_t index)
{
    if (bsIncludeDecoded[index] != NULL) {
        return bsIncludeDecoded[index];
    }
    const BSIncludeSource *source = &bsIncludeSources[index];
    bsIncludeDecoded[index] = bsGzipUncompress(source->gzip, source->gzipSize);
    return bsIncludeDecoded[index];
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
    return ix < BS_INCLUDE_COUNT ? bsIncludeSourceDecode(ix) : NULL;
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
    const char *text = bsIncludeSourceDecode(ix);
    if (text == NULL) {
        return NULL; /* the include is compiled out */
    }
    BSScript *script = bsScriptFromModelJSON(text, strlen(text), name, NULL);
    free(bsIncludeDecoded[ix]);
    bsIncludeDecoded[ix] = NULL;
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
        free(bsIncludeDecoded[ix]);
        bsIncludeDecoded[ix] = NULL;
        if (bsIncludeScripts[ix] != NULL) {
            bsScriptRelease(bsIncludeScripts[ix]);
            bsIncludeScripts[ix] = NULL;
        }
    }
}
