/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The bundled BareScript include library
 *
 * Each include library script is embedded in the library as its parser-compiled binary script
 * model, deflated (see bin/includeSource.bare). A model is inflated on first use; its compiled
 * script is cached, and the bytes are kept only for bsIncludeSource's callers.
 */

#include <stdlib.h>
#include <string.h>

#include "barescript/includeSource.h"

#include "internal.h"


/* A shared string's span in the inflated shared string table */
typedef struct {
    uint32_t offset;
    uint32_t length;
} BSIncludeSharedSpan;

/* The thread's compiled include scripts, inflated models, and shared string table - the registry itself is never written */
static _Thread_local struct {
    BSScript *scripts[BS_INCLUDE_COUNT];
    unsigned char *decoded[BS_INCLUDE_COUNT];
    size_t decodedSize[BS_INCLUDE_COUNT];
    unsigned char *sharedText;  /* the shared string table, inflated on the first reference */
    BSIncludeSharedSpan *sharedSpans;
    BSValue *sharedStrings;     /* interned on first use, else null */
    size_t sharedCount;
} bsIncludeTS;


/*
 * Bit reader - DEFLATE packs bits LSB-first within each byte
 */

typedef struct {
    const unsigned char *data;
    size_t size;
    size_t offset;
    unsigned bitBuf;
    int bitCount;
} BSBits;


/* Fill the bit buffer to "n" bits where the input allows; bitCount is how many it then holds */
static void bsBitsFill(BSBits *bits, int n)
{
    while (bits->bitCount < n && bits->offset < bits->size) {
        bits->bitBuf |= (unsigned) bits->data[bits->offset++] << bits->bitCount;
        bits->bitCount += 8;
    }
}


static int bsGetBits(BSBits *bits, int n)
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
 * A canonical Huffman code (RFC 1951, 3.2.2) as a decoding table: the codes of up to nine bits
 * in a table indexed by the next nine stream bits, in stream order, holding each code's length
 * and symbol; the longer codes - rare, and absent from the fixed code - decode a bit at a time
 * from the per-length counts and the symbols in code order.
 */
#define BS_HUFFMAN_TABLE_BITS 9
#define BS_HUFFMAN_TABLE_SIZE (1u << BS_HUFFMAN_TABLE_BITS)
#define BS_HUFFMAN_MAX_BITS 15
#define BS_HUFFMAN_MAX_SYMBOLS 288

typedef struct {
    uint16_t table[BS_HUFFMAN_TABLE_SIZE];
    uint16_t count[BS_HUFFMAN_MAX_BITS + 1];
    uint16_t symbols[BS_HUFFMAN_MAX_SYMBOLS];
} BSHuffman;


/* Build the decoding table from the symbols' code lengths; false for an over-subscribed code */
static bool bsHuffmanBuild(BSHuffman *huffman, const unsigned char *lengths, unsigned symbolCount)
{
    memset(huffman->count, 0, sizeof(huffman->count));
    for (unsigned symbol = 0; symbol < symbolCount; symbol++) {
        huffman->count[lengths[symbol]]++;
    }
    huffman->count[0] = 0;
    int left = 1;
    uint16_t offsets[BS_HUFFMAN_MAX_BITS + 1];
    offsets[1] = 0;
    for (unsigned length = 1; length <= BS_HUFFMAN_MAX_BITS; length++) {
        left = left * 2 - huffman->count[length];
        if (left < 0) {
            return false;
        }
        if (length < BS_HUFFMAN_MAX_BITS) {
            offsets[length + 1] = (uint16_t) (offsets[length] + huffman->count[length]);
        }
    }
    for (unsigned symbol = 0; symbol < symbolCount; symbol++) {
        if (lengths[symbol] != 0) {
            huffman->symbols[offsets[lengths[symbol]]++] = (uint16_t) symbol;
        }
    }

    /* The table: each short code's bits reversed into stream order, repeated past its length */
    memset(huffman->table, 0, sizeof(huffman->table));
    unsigned code = 0;
    unsigned index = 0;
    for (unsigned length = 1; length <= BS_HUFFMAN_TABLE_BITS; length++) {
        for (unsigned ix = 0; ix < huffman->count[length]; ix++, index++, code++) {
            unsigned reversed = 0;
            for (unsigned bit = 0; bit < length; bit++) {
                reversed |= ((code >> bit) & 1u) << (length - 1 - bit);
            }
            for (unsigned at = reversed; at < BS_HUFFMAN_TABLE_SIZE; at += 1u << length) {
                huffman->table[at] = (uint16_t) ((length << BS_HUFFMAN_TABLE_BITS) | huffman->symbols[index]);
            }
        }
        code <<= 1;
    }
    return true;
}


/* Decode a symbol; -1 at the end of the input or for a code the table does not hold */
static int bsHuffmanDecode(BSBits *bits, const BSHuffman *huffman)
{
    bsBitsFill(bits, BS_HUFFMAN_MAX_BITS);
    unsigned entry = huffman->table[bits->bitBuf & (BS_HUFFMAN_TABLE_SIZE - 1)];
    if (entry != 0) {
        int length = (int) (entry >> BS_HUFFMAN_TABLE_BITS);
        if (length > bits->bitCount) {
            return -1;
        }
        bits->bitBuf >>= length;
        bits->bitCount -= length;
        return (int) (entry & (BS_HUFFMAN_TABLE_SIZE - 1));
    }

    /* A code longer than the table: walk the code lengths, one stream bit at a time */
    int code = 0;
    int first = 0;
    int index = 0;
    for (int length = 1; length <= BS_HUFFMAN_MAX_BITS; length++) {
        if (length > bits->bitCount) {
            return -1;
        }
        code |= (int) ((bits->bitBuf >> (length - 1)) & 1u);
        int count = huffman->count[length];
        if (code - count < first) {
            bits->bitBuf >>= length;
            bits->bitCount -= length;
            return huffman->symbols[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
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

/* The order the code length code's lengths arrive in (RFC 1951, 3.2.7) */
static const unsigned char bsCodeLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};


/* Decode one block's codes into "out" past "outLen" bytes. Returns the output length, or SIZE_MAX for a malformed stream. */
static size_t bsInflateCodes(BSBits *bits, unsigned char *out, size_t outLen, size_t outCap,
                             const BSHuffman *literals, const BSHuffman *distances)
{
    for (;;) {
        int symbol = bsHuffmanDecode(bits, literals);
        if (symbol < 0) {
            return SIZE_MAX;
        }
        if (symbol < 256) {
            if (outLen >= outCap) {
                return SIZE_MAX;
            }
            out[outLen++] = (unsigned char) symbol;
            continue;
        }
        if (symbol == 256) {
            return outLen;
        }
        if (symbol > 285) {
            return SIZE_MAX;
        }
        int extra = bsGetBits(bits, bsLengthExtra[symbol - 257]);
        if (extra < 0) {
            return SIZE_MAX;
        }
        unsigned length = (unsigned) (bsLengthBase[symbol - 257] + extra);
        int distSymbol = bsHuffmanDecode(bits, distances);
        if (distSymbol < 0 || distSymbol > 29) {
            return SIZE_MAX;
        }
        extra = bsGetBits(bits, bsDistExtra[distSymbol]);
        if (extra < 0) {
            return SIZE_MAX;
        }
        unsigned distance = (unsigned) (bsDistBase[distSymbol] + extra);
        if (distance > outLen || outLen + length > outCap) {
            return SIZE_MAX;
        }
        size_t src = outLen - distance;
        if (distance >= length) {
            memcpy(out + outLen, out + src, length);
            outLen += length;
        } else {
            /* The match overlaps its own output - it repeats the last "distance" bytes */
            for (unsigned ix = 0; ix < length; ix++) {
                out[outLen++] = out[src + ix];
            }
        }
    }
}


/* Read a dynamic block's code lengths (RFC 1951, 3.2.7) and build its two codes; false if malformed */
static bool bsInflateDynamicCodes(BSBits *bits, BSHuffman *literals, BSHuffman *distances)
{
    int literalCount = bsGetBits(bits, 5);
    int distanceCount = bsGetBits(bits, 5);
    int codeCount = bsGetBits(bits, 4);
    if (literalCount < 0 || distanceCount < 0 || codeCount < 0 || literalCount > 29) {
        return false;
    }
    literalCount += 257;
    distanceCount += 1;
    codeCount += 4;

    /* The code length code, then the two codes' lengths under it, with the run-length symbols -
     * sized for the most the header can name: 286 literal-length codes and 32 distance codes */
    unsigned char lengths[286 + 32];
    memset(lengths, 0, 19);
    for (int ix = 0; ix < codeCount; ix++) {
        int length = bsGetBits(bits, 3);
        if (length < 0) {
            return false;
        }
        lengths[bsCodeLengthOrder[ix]] = (unsigned char) length;
    }
    BSHuffman codeLengths;
    if (!bsHuffmanBuild(&codeLengths, lengths, 19)) {
        return false;
    }
    int total = literalCount + distanceCount;
    int count = 0;
    while (count < total) {
        int symbol = bsHuffmanDecode(bits, &codeLengths);
        if (symbol < 0) {
            return false;
        }
        if (symbol < 16) {
            lengths[count++] = (unsigned char) symbol;
            continue;
        }
        unsigned char repeat = 0;
        int runLength;
        if (symbol == 16) {
            if (count == 0) {
                return false;
            }
            repeat = lengths[count - 1];
            runLength = bsGetBits(bits, 2) + 3;
        } else if (symbol == 17) {
            runLength = bsGetBits(bits, 3) + 3;
        } else {
            runLength = bsGetBits(bits, 7) + 11;
        }
        if (count + runLength > total) {
            return false;
        }
        memset(lengths + count, repeat, (size_t) runLength);
        count += runLength;
    }
    return bsHuffmanBuild(literals, lengths, (unsigned) literalCount) &&
           bsHuffmanBuild(distances, lengths + literalCount, (unsigned) distanceCount);
}


/* The fixed codes (RFC 1951, 3.2.6): 7 bits code 256-279, 8 bits 0-143 and 280-287, 9 bits 144-255; distances 5 bits */
static void bsInflateFixedCodes(BSHuffman *literals, BSHuffman *distances)
{
    unsigned char lengths[288];
    memset(lengths, 8, 144);
    memset(lengths + 144, 9, 112);
    memset(lengths + 256, 7, 24);
    memset(lengths + 280, 8, 8);
    bsHuffmanBuild(literals, lengths, 288);
    memset(lengths, 5, 32);
    bsHuffmanBuild(distances, lengths, 32);
}


static uint32_t bsReadU32LE(const unsigned char *data)
{
    return (uint32_t) data[0] | ((uint32_t) data[1] << 8) | ((uint32_t) data[2] << 16) |
           ((uint32_t) data[3] << 24);
}


/*
 * The bundled models are written by bin/includeSource.bare's compressor: the ten-byte gzip header
 * with no optional fields, blocks of fixed or dynamic Huffman codes, and the CRC and size trailer.
 * A stored block is not decoded - the compressor never writes one. The data is compiled in, so the
 * inflated size is the one integrity check it needs; the CRC is not verified. The output is
 * NUL-terminated past its size.
 */
unsigned char *bsGzipUncompress(const unsigned char *src, size_t srcSize, size_t *size)
{
    *size = 0;
    if (srcSize < 18 || src[0] != 0x1f || src[1] != 0x8b || src[2] != 8 || src[3] != 0) {
        return NULL;
    }
    uint32_t isize = bsReadU32LE(src + srcSize - 4);
    unsigned char *out = bsAlloc((size_t) isize + 1);
    BSBits bits = {src + 10, srcSize - 18, 0, 0, 0};
    BSHuffman literals;
    BSHuffman distances;
    size_t outLen = 0;
    int final;
    do {
        final = bsGetBits(&bits, 1);
        int type = bsGetBits(&bits, 2);
        if (type == 1) {
            bsInflateFixedCodes(&literals, &distances);
        } else if (type != 2 || !bsInflateDynamicCodes(&bits, &literals, &distances)) {
            goto fail;
        }
        outLen = bsInflateCodes(&bits, out, outLen, isize, &literals, &distances);
        if (outLen == SIZE_MAX) {
            goto fail;
        }
    } while (final == 0);
    if (outLen != isize) {
        goto fail;
    }
    out[isize] = '\0';
    *size = isize;
    return out;
fail:
    free(out);
    return NULL;
}


const unsigned char *bsIncludeSourceDecode(size_t index, size_t *size)
{
    if (bsIncludeTS.decoded[index] == NULL) {
        const BSIncludeSource *source = &bsIncludeSources[index];
        bsIncludeTS.decoded[index] = bsGzipUncompress(source->gzip, source->gzipSize, &bsIncludeTS.decodedSize[index]);
    }
    *size = bsIncludeTS.decodedSize[index];
    return bsIncludeTS.decoded[index];
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


const unsigned char *bsIncludeSource(const char *name, size_t *size)
{
    size_t ix = bsIncludeFind(name);
    if (ix == BS_INCLUDE_COUNT) {
        *size = 0;
        return NULL;
    }
    return bsIncludeSourceDecode(ix, size);
}


BSScript *bsIncludeScript(const char *name)
{
    size_t ix = bsIncludeFind(name);
    if (ix == BS_INCLUDE_COUNT) {
        return NULL;
    }
    if (bsIncludeTS.scripts[ix] != NULL) {
        return bsScriptRetain(bsIncludeTS.scripts[ix]);
    }
    size_t size;
    const unsigned char *model = bsIncludeSourceDecode(ix, &size);
    if (model == NULL) {
        return NULL; /* the include is compiled out */
    }
    BSScript *script = bsScriptFromModelBinary(model, size, name);
    free(bsIncludeTS.decoded[ix]);
    bsIncludeTS.decoded[ix] = NULL;
    if (script == NULL) {
        return NULL; /* GCOV_EXCL_LINE - bundled models always convert */
    }
    bsIncludeTS.scripts[ix] = bsScriptRetain(script);
    return script;
}


/* A varint of the shared table's text - well-formed, being generated */
static size_t bsIncludeSharedVarint(const unsigned char **text)
{
    size_t value = 0;
    unsigned shift = 0;
    unsigned char byte;
    do {
        byte = *(*text)++;
        value |= (size_t) (byte & 0x7f) << shift;
        shift += 7;
    } while ((byte & 0x80) != 0);
    return value;
}


bool bsIncludeSharedString(size_t index, BSValue *string)
{
    if (bsIncludeTS.sharedText == NULL) {
        size_t size;
        bsIncludeTS.sharedText = bsGzipUncompress(bsIncludeSourceShared.gzip, bsIncludeSourceShared.gzipSize, &size);
        const unsigned char *text = bsIncludeTS.sharedText;
        bsIncludeTS.sharedCount = bsIncludeSharedVarint(&text);
        bsIncludeTS.sharedSpans = bsAlloc(bsIncludeTS.sharedCount * sizeof(BSIncludeSharedSpan));
        bsIncludeTS.sharedStrings = bsAlloc(bsIncludeTS.sharedCount * sizeof(BSValue));
        for (size_t ix = 0; ix < bsIncludeTS.sharedCount; ix++) {
            size_t length = bsIncludeSharedVarint(&text);
            bsIncludeTS.sharedSpans[ix].offset = (uint32_t) (text - bsIncludeTS.sharedText);
            bsIncludeTS.sharedSpans[ix].length = (uint32_t) length;
            bsIncludeTS.sharedStrings[ix] = bsNull();
            text += length;
        }
    }
    if (index >= bsIncludeTS.sharedCount) {
        return false;
    }
    if (!bsIsType(bsIncludeTS.sharedStrings[index], BS_STRING)) {
        const BSIncludeSharedSpan *span = &bsIncludeTS.sharedSpans[index];
        bsIncludeTS.sharedStrings[index] = bsStringIntern((const char *) bsIncludeTS.sharedText + span->offset, span->length);
    }
    *string = bsIncludeTS.sharedStrings[index];
    return true;
}


void bsIncludeCleanup(void)
{
    for (size_t ix = 0; ix < bsIncludeTS.sharedCount; ix++) {
        bsRelease(bsIncludeTS.sharedStrings[ix]);
    }
    free(bsIncludeTS.sharedStrings);
    free(bsIncludeTS.sharedSpans);
    free(bsIncludeTS.sharedText);
    for (size_t ix = 0; ix < BS_INCLUDE_COUNT; ix++) {
        free(bsIncludeTS.decoded[ix]);
        if (bsIncludeTS.scripts[ix] != NULL) {
            bsScriptRelease(bsIncludeTS.scripts[ix]);
        }
    }
    memset(&bsIncludeTS, 0, sizeof(bsIncludeTS));
}
