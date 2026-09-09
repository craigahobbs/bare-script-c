/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The BareScript value system
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "barescript/json.h"

#define BARESCRIPT_VALUE_IMPL
#include "internal.h"


/*
 * Memory allocation
 *
 * BareScript treats allocation failure as fatal - there is no useful way for a script runtime to
 * continue without memory, and threading an out-of-memory result through every value operation
 * would obscure the code for a case that cannot be tested.
 */

/* GCOV_EXCL_START */
static void bsOutOfMemory(void)
{
    fputs("BareScript: out of memory\n", stderr);
    abort();
}
/* GCOV_EXCL_STOP */


/*
 * The allocation wrappers are the runtime's only calls to malloc and realloc, and they stay out of
 * line on purpose: macOS's xzone allocator pools untyped allocations by call site, hashed with a key
 * drawn at random for each process, and chunks emptied by one pool are not reused by another until
 * the kernel reclaims them. Inlined, the wrappers became more than a hundred call sites, and a
 * workload that frees strings made by one and allocates the same sizes through another kept or
 * dropped tens of megabytes at random per process. One call site keeps every allocation in one pool.
 */
BS_NOINLINE void *bsAlloc(size_t size)
{
    void *ptr = malloc(size);
    /* GCOV_EXCL_START */
    if (ptr == NULL) {
        bsOutOfMemory();
    }
    /* GCOV_EXCL_STOP */
    return ptr;
}


BS_NOINLINE void *bsRealloc(void *ptr, size_t size)
{
    void *result = realloc(ptr, size);
    /* GCOV_EXCL_START */
    if (result == NULL) {
        bsOutOfMemory();
    }
    /* GCOV_EXCL_STOP */
    return result;
}


char *bsStrdup(const char *text)
{
    size_t size = strlen(text) + 1;
    char *result = bsAlloc(size);
    memcpy(result, text, size);
    return result;
}


/*
 * The thread's value state
 *
 * The free lists and the intern table are per thread, so threads never
 * share a value and never contend - see README's "Threads". They are one struct so a function that
 * touches several of them computes the thread-local address once.
 */
#define BS_STRING_POOL_CLASSES 5
#define BS_ARRAY_BUF_CLASS_COUNT 6
#define BS_ENTRY_POOL_CLASS_COUNT 2

typedef struct {
    BSString *string;
    uint32_t hash;
} BSInternSlot;

/* The scratch a string builder starts in - past it, and for the results the pool does not take, a heap block */
#define BS_SB_SCRATCH 256

typedef struct {
    BSString *stringPool[BS_STRING_POOL_CLASSES];
    unsigned stringPoolCount[BS_STRING_POOL_CLASSES];
    BSArray *arrayPool;
    unsigned arrayPoolCount;
    BSValue *arrayBufPool[BS_ARRAY_BUF_CLASS_COUNT];
    unsigned arrayBufPoolCount[BS_ARRAY_BUF_CLASS_COUNT];
    BSObject *objectPool;
    unsigned objectPoolCount;
    BSObjectEntry *entryPool[BS_ENTRY_POOL_CLASS_COUNT];
    unsigned entryPoolCount[BS_ENTRY_POOL_CLASS_COUNT];
    BSInternSlot *internSlots; /* NULL until the thread's first intern */
    size_t internMask;
    size_t internCount;
    BSString *shortStrings[128 + 1]; /* the empty string, then each one-byte ASCII string, once created */
    char sbScratch[BS_SB_SCRATCH];  /* where a string builder's small result takes shape - see bsSBReserve */
    bool sbScratchBusy;
} BSValueState;

static _Thread_local BSValueState bsTS;


/* Put an item on a pool's free list - "next" is the item's link - or free it when the list holds "max" */
#define BS_POOL_GIVE(head, count, max, item, next) \
    do { \
        if ((count) < (max)) { \
            (next) = (void *) (head); \
            (head) = (item); \
            (count)++; \
        } else { \
            free(item); \
        } \
    } while (0)


/* Free every item on a pool's free list - "next" is the item's link, through "item" - and zero its count */
#define BS_POOL_DRAIN(head, count, type, next) \
    do { \
        while ((head) != NULL) { \
            type *item = (head); \
            (head) = (type *) (next); \
            free(item); \
        } \
        (count) = 0; \
    } while (0)


/*
 * Value constructors
 */


BSValue bsNull(void)
{
    return (BSValue) {.type = BS_NULL, .u.ref = NULL};
}


BSValue bsBoolean(bool boolean)
{
    return (BSValue) {.type = BS_BOOLEAN, .u.boolean = boolean};
}


BSValue bsNumber(double number)
{
    return (BSValue) {.type = BS_NUMBER, .u.number = number};
}


BSValue bsDatetime(int64_t milliseconds)
{
    return (BSValue) {.type = BS_DATETIME, .u.datetime = milliseconds};
}


/*
 * UTF-8
 */


size_t bsUTF8Encode(uint32_t codePoint, char *buffer)
{
    if (codePoint < 0x80) {
        buffer[0] = (char) codePoint;
        return 1;
    }
    if (codePoint < 0x800) {
        buffer[0] = (char) (0xC0 | (codePoint >> 6));
        buffer[1] = (char) (0x80 | (codePoint & 0x3F));
        return 2;
    }
    if (codePoint < 0x10000) {
        buffer[0] = (char) (0xE0 | (codePoint >> 12));
        buffer[1] = (char) (0x80 | ((codePoint >> 6) & 0x3F));
        buffer[2] = (char) (0x80 | (codePoint & 0x3F));
        return 3;
    }
    buffer[0] = (char) (0xF0 | (codePoint >> 18));
    buffer[1] = (char) (0x80 | ((codePoint >> 12) & 0x3F));
    buffer[2] = (char) (0x80 | ((codePoint >> 6) & 0x3F));
    buffer[3] = (char) (0x80 | (codePoint & 0x3F));
    return 4;
}


uint32_t bsUTF8Decode(const char *data, size_t size, size_t offset, size_t *codeSize)
{
    const unsigned char *bytes = (const unsigned char *) data;
    unsigned char lead = bytes[offset];
    size_t extra;
    uint32_t codePoint;

    if (lead < 0x80) {
        *codeSize = 1;
        return lead;
    }
    if ((lead & 0xE0) == 0xC0) {
        extra = 1;
        codePoint = lead & 0x1Fu;
    } else if ((lead & 0xF0) == 0xE0) {
        extra = 2;
        codePoint = lead & 0x0Fu;
    } else if ((lead & 0xF8) == 0xF0) {
        extra = 3;
        codePoint = lead & 0x07u;
    } else {
        /* An invalid lead byte decodes as the replacement character */
        *codeSize = 1;
        return 0xFFFD;
    }

    if (offset + extra >= size) {
        *codeSize = 1;
        return 0xFFFD;
    }
    for (size_t ix = 1; ix <= extra; ix++) {
        unsigned char next = bytes[offset + ix];
        if ((next & 0xC0) != 0x80) {
            *codeSize = 1;
            return 0xFFFD;
        }
        codePoint = (codePoint << 6) | (next & 0x3Fu);
    }
    *codeSize = extra + 1;
    return codePoint;
}


/*
 * Count a UTF-8 buffer's code points, validating strictly
 *
 * Overlong encodings, surrogate code points, and code points above U+10FFFF are rejected - the
 * same set the reference implementations reject, since their strings are validated Unicode.
 */
size_t bsUTF8Length(const char *data, size_t size)
{
    size_t length = 0;
    size_t offset = 0;
    while (offset < size) {
        unsigned char lead = (unsigned char) data[offset];
        size_t extra;
        unsigned char secondLow = 0x80;
        unsigned char secondHigh = 0xBF;
        if (lead < 0x80) {
            extra = 0;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            extra = 1;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            extra = 2;
            if (lead == 0xE0) {
                secondLow = 0xA0;
            } else if (lead == 0xED) {
                secondHigh = 0x9F;
            }
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            extra = 3;
            if (lead == 0xF0) {
                secondLow = 0x90;
            } else if (lead == 0xF4) {
                secondHigh = 0x8F;
            }
        } else {
            return SIZE_MAX;
        }
        if (offset + extra >= size) {
            return SIZE_MAX;
        }
        for (size_t ix = 1; ix <= extra; ix++) {
            unsigned char next = (unsigned char) data[offset + ix];
            unsigned char low = (ix == 1 ? secondLow : 0x80);
            unsigned char high = (ix == 1 ? secondHigh : 0xBF);
            if (next < low || next > high) {
                return SIZE_MAX;
            }
        }
        offset += extra + 1;
        length++;
    }
    return length;
}


/*
 * Numbers - formatting per JavaScript's Number.prototype.toString, and parsing
 */


/* Copy formatted text into the caller's buffer, NUL-terminated and truncated as snprintf would */
static size_t bsNumberEmit(char *buffer, size_t bufferSize, const char *text, size_t size)
{
    if (bufferSize != 0) {
        size_t copy = size < bufferSize - 1 ? size : bufferSize - 1;
        memcpy(buffer, text, copy);
        buffer[copy] = '\0';
    }
    return size;
}


/* A non-integer, or one past the fixed range: NaN, an infinity, or the shortest round-tripping digits */
static BS_NOINLINE size_t bsNumberFormatSlow(double number, char *buffer, size_t bufferSize)
{
    if (isnan(number)) {
        return bsNumberEmit(buffer, bufferSize, "NaN", 3);
    }
    if (isinf(number)) {
        return number > 0 ? bsNumberEmit(buffer, bufferSize, "Infinity", 8) :
            bsNumberEmit(buffer, bufferSize, "-Infinity", 9);
    }

    /*
     * Find the shortest round-tripping decimal representation
     *
     * Round-tripping is monotone in precision - if p digits round-trip then so do p + 1 - so the
     * shortest precision is found by binary search rather than by trying each in turn.
     */
    char digits[40];
    int low = 1;
    int high = 17;
    while (low < high) {
        int middle = low + (high - low) / 2;
        snprintf(digits, sizeof(digits), "%.*e", middle - 1, number);
        if (strtod(digits, NULL) == number) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }
    snprintf(digits, sizeof(digits), "%.*e", low - 1, number);

    /* Split the "d.dddde+XX" form into its digits and exponent */
    char mantissa[24];
    size_t digitCount = 0;
    const char *cursor = digits;
    bool negative = false;
    if (*cursor == '-') {
        negative = true;
        cursor++;
    }
    for (; *cursor != '\0' && *cursor != 'e'; cursor++) {
        if (*cursor != '.') {
            mantissa[digitCount++] = *cursor;
        }
    }
    int exponent = (int) strtol(cursor + 1, NULL, 10);

    /*
     * Strip trailing zeroes from the mantissa
     *
     * The shortest round-tripping representation cannot end in a zero - dropping it would give the
     * same value at one less precision, which the search above would have found first - so this is
     * defensive against a libc whose rounding disagrees.
     */
    /* GCOV_EXCL_START */
    while (digitCount > 1 && mantissa[digitCount - 1] == '0') {
        digitCount--;
    }
    /* GCOV_EXCL_STOP */

    /* Format per the ECMAScript Number::toString algorithm - "n" is the decimal point position */
    int n = exponent + 1;
    int k = (int) digitCount;
    char text[64];
    size_t size = 0;
    if (negative) {
        text[size++] = '-';
    }
    if (k <= n && n <= 21) {
        memcpy(text + size, mantissa, (size_t) k);
        size += (size_t) k;
        for (int ix = 0; ix < n - k; ix++) {
            text[size++] = '0';
        }
    } else if (0 < n && n <= 21) {
        memcpy(text + size, mantissa, (size_t) n);
        size += (size_t) n;
        text[size++] = '.';
        memcpy(text + size, mantissa + n, (size_t) (k - n));
        size += (size_t) (k - n);
    } else if (-6 < n && n <= 0) {
        text[size++] = '0';
        text[size++] = '.';
        for (int ix = 0; ix < -n; ix++) {
            text[size++] = '0';
        }
        memcpy(text + size, mantissa, (size_t) k);
        size += (size_t) k;
    } else {
        text[size++] = mantissa[0];
        if (k > 1) {
            text[size++] = '.';
            memcpy(text + size, mantissa + 1, (size_t) (k - 1));
            size += (size_t) (k - 1);
        }
        size += (size_t) snprintf(text + size, sizeof(text) - size, "e%+d", n - 1);
    }
    return bsNumberEmit(buffer, bufferSize, text, size);
}


/*
 * Format a number: an integer that prints without an exponent - the common case by far - by a
 * digit loop in place, anything else through the round-trip search, which is kept out of line so
 * the string concatenation, append, join, and JSON encoder bodies that inline this stay small
 */
static inline size_t bsNumberFormatFast(double number, char *buffer, size_t bufferSize)
{
    if (number == trunc(number) && number > -1e15 && number < 1e15) {
        char text[24];
        size_t begin = sizeof(text);
        bool negative = number < 0;
        uint64_t magnitude = (uint64_t) (negative ? -number : number);
        do {
            text[--begin] = (char) ('0' + (magnitude % 10));
            magnitude /= 10;
        } while (magnitude != 0);
        if (negative) {
            text[--begin] = '-';
        }
        return bsNumberEmit(buffer, bufferSize, text + begin, sizeof(text) - begin);
    }
    return bsNumberFormatSlow(number, buffer, bufferSize);
}


size_t bsNumberFormat(double number, char *buffer, size_t bufferSize)
{
    return bsNumberFormatFast(number, buffer, bufferSize);
}


bool bsNumberRound(double number, double digits, double *result)
{
    double multiplier = pow(10, digits);
    double rounded = trunc(number * multiplier + (number >= 0 ? 0.5 : -0.5)) / multiplier;
    if (!isfinite(rounded)) {
        return false;
    }
    *result = rounded;
    return true;
}


/*
 * strtod over an unterminated span, copied out since strtod needs a terminator. Almost every
 * number fits the stack buffer; a longer one - a very long run of digits - takes a heap copy.
 */
double bsStrtod(const char *text, size_t size)
{
    char buffer[64];
    char *number = size < sizeof(buffer) ? buffer : bsAlloc(size + 1);
    memcpy(number, text, size);
    number[size] = '\0';
    double value = strtod(number, NULL);
    if (number != buffer) {
        free(number);
    }
    return value;
}


/* The offset past the Unicode spaces at "ix" */
static size_t bsSkipSpaces(const char *text, size_t size, size_t ix)
{
    while (ix < size) {
        size_t codeSize;
        if (!bsIsSpaceCode(bsUTF8Decode(text, size, ix, &codeSize))) {
            break;
        }
        ix += codeSize;
    }
    return ix;
}


bool bsNumberParse(const char *text, size_t size, double *result)
{
    /* ^\s*[-+]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][-+]?[0-9]+)?\s*$ */
    size_t ix = bsSkipSpaces(text, size, 0);
    size_t begin = ix;
    if (ix < size && (text[ix] == '-' || text[ix] == '+')) {
        ix++;
    }
    size_t integerDigits = 0;
    while (ix < size && text[ix] >= '0' && text[ix] <= '9') {
        ix++;
        integerDigits++;
    }
    size_t fractionDigits = 0;
    if (ix < size && text[ix] == '.') {
        ix++;
        while (ix < size && text[ix] >= '0' && text[ix] <= '9') {
            ix++;
            fractionDigits++;
        }
    }
    if (integerDigits == 0 && fractionDigits == 0) {
        return false;
    }
    if (ix < size && (text[ix] == 'e' || text[ix] == 'E')) {
        ix++;
        if (ix < size && (text[ix] == '-' || text[ix] == '+')) {
            ix++;
        }
        size_t exponentDigits = 0;
        while (ix < size && text[ix] >= '0' && text[ix] <= '9') {
            ix++;
            exponentDigits++;
        }
        if (exponentDigits == 0) {
            return false;
        }
    }
    size_t end = ix;
    ix = bsSkipSpaces(text, size, ix);
    if (ix != size) {
        return false;
    }

    double value = bsStrtod(text + begin, end - begin);
    if (!isfinite(value)) {
        return false;
    }
    *result = value;
    return true;
}


bool bsIntegerParse(const char *text, size_t size, int radix, double *result)
{
    if (radix < 2 || radix > 36) {
        return false;
    }
    size_t ix = bsSkipSpaces(text, size, 0);
    bool negative = false;
    if (ix < size && (text[ix] == '-' || text[ix] == '+')) {
        negative = (text[ix] == '-');
        ix++;
    }
    size_t digits = 0;
    double value = 0;
    while (ix < size) {
        int digit = bsDigitValue(text[ix]);
        if (digit < 0 || digit >= radix) {
            break;
        }
        value = value * radix + digit;
        digits++;
        ix++;
    }
    if (digits == 0) {
        return false;
    }
    ix = bsSkipSpaces(text, size, ix);
    if (ix != size || !isfinite(value)) {
        return false;
    }
    *result = negative ? -value : value;
    return true;
}


/*
 * String values
 */


/*
 * Small string recycling
 *
 * Strings are the runtime's most frequent allocation - a match group, a slice, a computed key -
 * and most are short. An allocation that fits one of five size classes is rounded up to it and
 * recycled through that class's free list; the class is kept in the string's flags so release
 * knows where the block goes. The free-list link reuses the index pointer.
 */
#define BS_STRING_POOL_MAX 4096
#define BS_STR_POOL_SHIFT 8
static const size_t bsStringPoolSize[BS_STRING_POOL_CLASSES] = {48, 64, 96, 128, 192};


/*
 * The storage after a string's header: its bytes, or for a slice its parent. A string outside the
 * pool - a block of its own, or bytes apart from a pooled header - opens its storage with the
 * capacity of the bytes that follow; a pooled string's capacity is its size class's.
 */
#define BS_STRING_STORAGE(string) ((char *) ((string) + 1))
#define BS_STRING_PARENT(string) (*(BSString **) ((string) + 1))
#define BS_STRING_HEAP_HEAD 8
#define BS_STRING_HEAP_CAPACITY(data) (*(uint32_t *) ((data) - BS_STRING_HEAP_HEAD))

/* A string's capacity: none for a slice, its pool class's, or the word its storage opens with */
static size_t bsStringCapacity(const BSString *string)
{
    uint16_t flags = string->flags;
    if ((flags & (BS_STR_SLICE | BS_STR_APART)) != 0) {
        return (flags & BS_STR_SLICE) != 0 ? 0 : BS_STRING_HEAP_CAPACITY(string->data);
    }
    unsigned class = flags >> BS_STR_POOL_SHIFT;
    return class != 0 ? bsStringPoolSize[class - 1] - sizeof(BSString) - 1 : BS_STRING_HEAP_CAPACITY(string->data);
}


/*
 * Set up a block of its own - past the pool - as a string of "size" data bytes with "capacity" of
 * them; the caller fills them and sets the length
 */
static void bsStringInitHeap(BSString *string, size_t size, size_t capacity)
{
    string->refcount = 1;
    string->flags = 0;
    string->size = (uint32_t) size;
    string->data = BS_STRING_STORAGE(string) + BS_STRING_HEAP_HEAD;
    BS_STRING_HEAP_CAPACITY(string->data) = (uint32_t) capacity;
    string->data[size] = '\0';
}

static BS_NOINLINE BSString *bsStringAllocHeap(size_t size)
{
    BSString *string = bsAlloc(sizeof(BSString) + BS_STRING_HEAP_HEAD + size + 1);
    bsStringInitHeap(string, size, size);
    return string;
}

/*
 * A string of "size" data bytes, from its size class's free list. Small enough to inline: a
 * constant size folds to one class, the class by a table over the total's sixteens.
 */
static inline BSString *bsStringAlloc(size_t size)
{
    static const uint8_t classOfSixteens[] = {0, 0, 0, 1, 2, 2, 3, 3, 4, 4, 4, 4};
    size_t total = sizeof(BSString) + size + 1;
    if (total > bsStringPoolSize[BS_STRING_POOL_CLASSES - 1]) {
        return bsStringAllocHeap(size);
    }
    unsigned ix = classOfSixteens[(total - 1) >> 4];
    BSString *string = bsTS.stringPool[ix];
    if (string != NULL) {
        bsTS.stringPool[ix] = (BSString *) string->index;
        bsTS.stringPoolCount[ix]--;
    } else {
        string = bsAlloc(bsStringPoolSize[ix]);
    }
    string->refcount = 1;
    string->flags = (uint16_t) ((ix + 1) << BS_STR_POOL_SHIFT);
    string->size = (uint32_t) size;
    string->data = BS_STRING_STORAGE(string);
    string->data[size] = '\0';
    return string;
}


static bool bsUTF8IsAscii(const char *data, size_t size)
{
    const unsigned char *bytes = (const unsigned char *) data;
    size_t ix = 0;
    while (ix + 8 <= size) {
        uint64_t word;
        memcpy(&word, bytes + ix, 8);
        if (word & 0x8080808080808080ULL) {
            return false;
        }
        ix += 8;
    }
    while (ix < size) {
        if (bytes[ix] & 0x80) {
            return false;
        }
        ix++;
    }
    return true;
}


static void bsStringFree(BSString *string)
{
    if ((string->flags & BS_STR_SLICE) != 0) {
        bsReleaseInline(bsStringTake(BS_STRING_PARENT(string)));
    } else if ((string->flags & BS_STR_APART) != 0) {
        free(string->data - BS_STRING_HEAP_HEAD);
    }
    if ((string->flags & BS_STR_INDEXED) != 0) {
        free(string->index);
    }
    unsigned class = string->flags >> BS_STR_POOL_SHIFT;
    if (class != 0) {
        BS_POOL_GIVE(bsTS.stringPool[class - 1], bsTS.stringPoolCount[class - 1], BS_STRING_POOL_MAX, string, string->index);
    } else {
        free(string);
    }
}


/*
 * The empty string and the one-byte ASCII strings are shared: a slice, a match group, or a split
 * piece of one character is the runtime's most frequent string, and each is created once per
 * thread and retained thereafter. The thread's reference keeps their count above one, so the
 * in-place append never sees one as unshared.
 */
static BSValue bsStringShort(const char *text, size_t size)
{
    BSString **slot = &bsTS.shortStrings[size == 0 ? 0 : 1 + (unsigned char) text[0]];
    BSString *string = *slot;
    if (string == NULL) {
        string = bsStringAlloc(size);
        memcpy(string->data, text, size);
        string->length = (uint32_t) size;
        *slot = string;
    }
    return bsRetainInline(bsStringTake(string));
}


BSValue bsStringNewAscii(const char *text, size_t size)
{
    if (size <= 1) {
        return bsStringShort(text, size);
    }
    BSString *string = bsStringAlloc(size);
    memcpy(string->data, text, size);
    string->length = (uint32_t) size;
    return bsStringTake(string);
}


/* The sharing half of bsStringSliceBytes: the span is long enough to slice */
BSValue bsStringSliceShare(BSValue parent, size_t offset, size_t size, size_t length)
{
    BSString *source = parent.u.string;
    char *text = source->data + offset;
    if (length == SIZE_MAX) {
        /* A span of a valid non-ASCII string at code point boundaries is valid itself */
        length = source->length == source->size ? size : bsUTF8Length(text, size);
    }

    /* The slice's storage holds the root parent, retained */
    BSString *root = (source->flags & BS_STR_SLICE) != 0 ? BS_STRING_PARENT(source) : source;
    BSString *slice = bsStringAlloc(sizeof(BSString *) - 1);
    slice->flags |= BS_STR_SLICE;
    slice->size = (uint32_t) size;
    slice->length = (uint32_t) length;
    slice->data = text;
    BS_STRING_PARENT(slice) = bsRetainInline(bsStringTake(root)).u.string;
    return bsStringTake(slice);
}


/* Give a slice bytes of its own, NUL-terminated, and let its parent go. Out of line: bsStringData is everywhere. */
static BS_NOINLINE void bsStringFlatten(BSString *string)
{
    char *copy = (char *) bsAlloc(BS_STRING_HEAP_HEAD + string->size + 1) + BS_STRING_HEAP_HEAD;
    BS_STRING_HEAP_CAPACITY(copy) = string->size;
    memcpy(copy, string->data, string->size);
    copy[string->size] = '\0';
    bsReleaseInline(bsStringTake(BS_STRING_PARENT(string)));
    string->data = copy;
    string->flags = (uint16_t) ((string->flags & ~BS_STR_SLICE) | BS_STR_APART);
}


/* Complete a string whose bytes are in place: count its code points and note if it is ASCII */
static BSValue bsStringFinish(BSString *string, size_t size)
{
    if (bsUTF8IsAscii(string->data, size)) {
        string->length = (uint32_t) size;
    } else {
        size_t length = bsUTF8Length(string->data, size);
        string->length = (uint32_t) (length != SIZE_MAX ? length : size);
    }
    return bsStringTake(string);
}


BSValue bsStringNewSize(const char *text, size_t size)
{
    if (size == 0 || (size == 1 && ((unsigned char) text[0]) < 0x80)) {
        return bsStringShort(text, size);
    }
    BSString *string = bsStringAlloc(size);
    memcpy(string->data, text, size);
    return bsStringFinish(string, size);
}


BSValue bsStringNew(const char *text)
{
    return bsStringNewSize(text, strlen(text));
}


BSValue bsStringTake(BSString *string)
{
    return (BSValue) {.type = BS_STRING, .u.string = string};
}


BSValue bsStringNewVFormat(const char *format, va_list args)
{
    va_list argsCopy;
    va_copy(argsCopy, args);
    int size = vsnprintf(NULL, 0, format, argsCopy);
    va_end(argsCopy);
    /* GCOV_EXCL_START */
    if (size < 0) {
        return bsStringNewSize("", 0);
    }
    /* GCOV_EXCL_STOP */

    BSString *string = bsStringAlloc((size_t) size);
    vsnprintf(string->data, (size_t) size + 1, format, args);
    return bsStringFinish(string, (size_t) size);
}


BSValue bsStringNewFormat(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    BSValue value = bsStringNewVFormat(format, args);
    va_end(args);
    return value;
}


/*
 * A value's string bytes - "text" is an owned string value for the types that format into a new
 * string, and null when the bytes point at the caller's buffer or at an existing string
 */
typedef struct BSStringBytes {
    const char *data;
    size_t size;
    size_t length;
    BSValue text;
} BSStringBytes;

/* Resolve a value to its string bytes, formatting a number into a caller-supplied buffer */
static BSStringBytes bsStringBytes(BSValue value, char *buffer, size_t bufferSize)
{
    switch (value.type) {
    case BS_STRING:
        return (BSStringBytes) {value.u.string->data, value.u.string->size, value.u.string->length, bsNull()};
    case BS_NUMBER: {
        size_t size = bsNumberFormatFast(value.u.number, buffer, bufferSize);
        return (BSStringBytes) {buffer, size, size, bsNull()};
    }
    case BS_NULL:
        return (BSStringBytes) {"null", 4, 4, bsNull()};
    case BS_BOOLEAN:
        return value.u.boolean ? (BSStringBytes) {"true", 4, 4, bsNull()} : (BSStringBytes) {"false", 5, 5, bsNull()};
    default: {
        BSValue text = bsValueString(value);
        return (BSStringBytes) {text.u.string->data, text.u.string->size, text.u.string->length, text};
    }
    }
}


BSValue bsStringConcat(BSValue left, BSValue right)
{
    char leftBuffer[64];
    char rightBuffer[64];
    BSStringBytes leftBytes = bsStringBytes(left, leftBuffer, sizeof(leftBuffer));
    BSStringBytes rightBytes = bsStringBytes(right, rightBuffer, sizeof(rightBuffer));

    BSString *string = bsStringAlloc(leftBytes.size + rightBytes.size);
    memcpy(string->data, leftBytes.data, leftBytes.size);
    memcpy(string->data + leftBytes.size, rightBytes.data, rightBytes.size);
    string->length = (uint32_t) (leftBytes.length + rightBytes.length);

    bsReleaseInline(leftBytes.text);
    bsReleaseInline(rightBytes.text);
    return bsStringTake(string);
}


BSString *bsStringAppendValue(BSString *string, BSValue value)
{
    char buffer[64];
    BSStringBytes bytes = bsStringBytes(value, buffer, sizeof(buffer));
    size_t newSize = string->size + bytes.size;
    size_t capacity = bsStringCapacity(string);
    if (newSize > capacity) {
        size_t total = sizeof(BSString) + newSize + 1;
        if (total <= 128 || (string->flags & (BS_STR_SLICE | BS_STR_APART)) != 0) {
            /*
             * Still a pooled size: move up a size class, block for block, with no allocator call.
             * A slice or a string with bytes apart from its header has none in its block, so it
             * starts over too.
             */
            BSString *grown = bsStringAlloc(newSize);
            memcpy(grown->data, string->data, string->size);
            grown->size = string->size;
            grown->length = string->length;
            bsStringFree(string);
            string = grown;
        } else {
            /*
             * Past the pool, a medium string grows to the allocator's own granularity, so a string
             * kept by the thousands carries no slack; a large one doubles, so a string built by
             * repeated appends copies each byte a bounded number of times. A recycled block that
             * outgrows its size class is freed like any other, its bytes moved past the capacity
             * word a block of its own opens with.
             */
            bool pooled = (string->flags >> BS_STR_POOL_SHIFT) != 0;
            capacity *= 2;
            if (total <= 256) {
                capacity = ((total + BS_STRING_HEAP_HEAD + 15) & ~(size_t) 15) - sizeof(BSString) - BS_STRING_HEAP_HEAD - 1;
            }
            while (capacity < newSize) {
                capacity *= 2;
            }
            string = bsRealloc(string, sizeof(BSString) + BS_STRING_HEAP_HEAD + capacity + 1);
            string->data = BS_STRING_STORAGE(string) + BS_STRING_HEAP_HEAD;
            if (pooled) {
                memmove(string->data, BS_STRING_STORAGE(string), string->size);
            }
            BS_STRING_HEAP_CAPACITY(string->data) = (uint32_t) capacity;
            string->flags &= (uint16_t) ((1u << BS_STR_POOL_SHIFT) - 1);
        }
    }
    memcpy(string->data + string->size, bytes.data, bytes.size);
    string->size = (uint32_t) newSize;
    string->length += (uint32_t) bytes.length;
    string->data[newSize] = '\0';
    if ((string->flags & BS_STR_INDEXED) != 0) {
        free(string->index);
    }
    string->flags &= (uint16_t) ~(BS_STR_HASHED | BS_STR_INDEXED);
    bsReleaseInline(bytes.text);
    return string;
}


const char *bsStringData(BSValue value)
{
    if (value.type != BS_STRING) {
        return "";
    }
    BSString *string = value.u.string;
    if ((string->flags & BS_STR_SLICE) != 0) {
        bsStringFlatten(string);
    }
    return string->data;
}


size_t bsStringSize(BSValue value)
{
    return value.type == BS_STRING ? value.u.string->size : 0;
}


size_t bsStringLength(BSValue value)
{
    return value.type == BS_STRING ? value.u.string->length : 0;
}


#define BS_STRING_INDEX_STRIDE 16

/*
 * A non-ASCII string's code point index, built on its first indexed access: a cursor - the last
 * code point index looked up and its byte offset - and, for a string long enough to want them, the
 * byte offset of every sixteenth code point, so a backward lookup starts near its target
 */
static uint32_t *bsStringIndex(BSString *string)
{
    if ((string->flags & BS_STR_INDEXED) != 0) {
        return string->index + 1;
    }
    size_t length = string->length;
    size_t markCount = length >= 2 * BS_STRING_INDEX_STRIDE ? length / BS_STRING_INDEX_STRIDE + 1 : 0;
    uint32_t *block = bsAlloc((3 + markCount) * sizeof(uint32_t));
    block[0] = string->hash;
    uint32_t *index = block + 1;
    index[0] = 0;
    index[1] = 0;
    if (markCount != 0) {
        uint32_t *offsets = index + 2;
        size_t offset = 0;
        size_t position = 0;
        size_t mark = 1;
        size_t next = BS_STRING_INDEX_STRIDE;
        offsets[0] = 0;
        while (position < length && mark < markCount) {
            size_t codeSize;
            bsUTF8Decode(string->data, string->size, offset, &codeSize);
            offset += codeSize;
            position++;
            if (position == next) {
                offsets[mark++] = (uint32_t) offset;
                next += BS_STRING_INDEX_STRIDE;
            }
        }
    }
    string->index = block;
    string->flags |= BS_STR_INDEXED;
    return index;
}


size_t bsStringOffsetSlow(BSValue value, size_t index)
{
    BSString *string = value.u.string;
    size_t length = string->length;
    if (index >= length) {
        return string->size;
    }

    uint32_t *cursor = bsStringIndex(string);
    size_t position = 0;
    size_t offset = 0;
    if (index >= cursor[0]) {
        position = cursor[0];
        offset = cursor[1];
    } else if (length >= 2 * BS_STRING_INDEX_STRIDE) {
        size_t mark = index / BS_STRING_INDEX_STRIDE;
        position = mark * BS_STRING_INDEX_STRIDE;
        offset = cursor[2 + mark];
    }
    while (offset < string->size && position < index) {
        size_t codeSize;
        bsUTF8Decode(string->data, string->size, offset, &codeSize);
        offset += codeSize;
        position++;
    }
    cursor[0] = (uint32_t) position;
    cursor[1] = (uint32_t) offset;
    return offset;
}


size_t bsStringOffset(BSValue value, size_t index)
{
    return bsStringOffsetFast(value, index);
}


uint32_t bsStringCodePoint(BSValue value, size_t index)
{
    size_t offset = bsStringOffset(value, index);
    if (offset >= bsStringSize(value)) {
        return 0;
    }
    size_t codeSize;
    return bsUTF8Decode(value.u.string->data, value.u.string->size, offset, &codeSize);
}


/*
 * Interned strings
 *
 * Short C-string keys (script names, bsObjectSet) and compiled names are interned so a lookup can
 * compare pointers. The table holds one reference; interned strings live until bsValueCleanup.
 * New intern entries stop at COUNT_MAX.
 */
#define BS_INTERN_MAX 64
#define BS_INTERN_INITIAL 32
#define BS_INTERN_COUNT_MAX 65536


/*
 * The content hash of a string's bytes
 *
 * FNV-1a a word at a time, then mixed: a multiply alone leaves the hash's low bits depending on
 * each word's low byte only, and the object index probes by the low bits, so keys that differ in
 * their other bytes - "/item/123", "10.0.0.7" - would share slots.
 */
static BS_NOINLINE uint32_t bsHashBytes(const char *data, size_t size)
{
    const unsigned char *bytes = (const unsigned char *) data;
    uint32_t hash = 2166136261u;
    size_t ix = 0;
    while (ix + 4 <= size) {
        uint32_t word;
        memcpy(&word, bytes + ix, 4);
        hash ^= word;
        hash *= 16777619u;
        ix += 4;
    }
    while (ix < size) {
        hash ^= bytes[ix++];
        hash *= 16777619u;
    }
    hash ^= hash >> 16;
    hash *= 0x85ebca6bu;
    hash ^= hash >> 13;
    hash *= 0xc2b2ae35u;
    hash ^= hash >> 16;
    return hash;
}


/* A string's content hash, computed once and kept on the string */
static uint32_t bsStringHash(BSString *string)
{
    uint32_t *hash = (string->flags & BS_STR_INDEXED) != 0 ? &string->index[0] : &string->hash;
    if ((string->flags & BS_STR_HASHED) == 0) {
        *hash = bsHashBytes(string->data, string->size);
        string->flags |= BS_STR_HASHED;
    }
    return *hash;
}


/* Store an interned string in the first empty slot of its probe sequence */
static void bsInternPut(BSString *string, uint32_t hash)
{
    for (size_t probe = 0;; probe++) {
        size_t slot = (hash + probe) & bsTS.internMask;
        if (bsTS.internSlots[slot].string == NULL) {
            bsTS.internSlots[slot].string = string;
            bsTS.internSlots[slot].hash = hash;
            return;
        }
    }
}


/* Double the table - or create it, at the thread's first intern */
static void bsInternGrow(void)
{
    BSInternSlot *old = bsTS.internSlots;
    size_t oldCapacity = old != NULL ? bsTS.internMask + 1 : 0;
    size_t capacity = old != NULL ? oldCapacity * 2 : BS_INTERN_INITIAL;
    bsTS.internMask = capacity - 1;
    bsTS.internSlots = bsAlloc(capacity * sizeof(BSInternSlot));
    memset(bsTS.internSlots, 0, capacity * sizeof(BSInternSlot));
    for (size_t ix = 0; ix < oldCapacity; ix++) {
        BSString *string = old[ix].string;
        if (string == NULL) {
            continue;
        }
        bsInternPut(string, old[ix].hash);
    }
    free(old);
}

static BSString *bsInternLookupHash(const char *data, size_t size, uint32_t hash)
{
    if (bsTS.internSlots == NULL) {
        return NULL;
    }
    for (size_t probe = 0;; probe++) {
        size_t slot = (hash + probe) & bsTS.internMask;
        BSString *string = bsTS.internSlots[slot].string;
        if (string == NULL) {
            return NULL;
        }
        if (bsTS.internSlots[slot].hash == hash && string->size == size &&
            (size == 0 || memcmp(string->data, data, size) == 0)) {
            return string;
        }
    }
}

BSValue bsStringIntern(const char *data, size_t size)
{
    if (size > BS_INTERN_MAX) {
        return bsStringNewSize(data, size);
    }
    uint32_t hash = bsHashBytes(data, size);
    BSString *found = bsInternLookupHash(data, size, hash);
    if (found != NULL) {
        return bsRetainInline(bsStringTake(found));
    }
    BSValue value = bsStringNewSize(data, size);
    value.u.string->hash = hash;
    value.u.string->flags |= BS_STR_HASHED;
    if (bsTS.internCount >= BS_INTERN_COUNT_MAX) {
        return value;
    }
    if ((bsTS.internCount + 1) * 4 >= (bsTS.internMask + 1) * 3) {
        bsInternGrow();
    }
    value.u.string->flags |= BS_STR_INTERNED;
    value.u.string->refcount++;
    bsInternPut(value.u.string, hash);
    bsTS.internCount++;
    return value;
}

BSValue bsStringInternExisting(const char *data, size_t size)
{
    if (size <= BS_INTERN_MAX) {
        BSString *found = bsInternLookupHash(data, size, bsHashBytes(data, size));
        if (found != NULL) {
            return bsRetainInline(bsStringTake(found));
        }
    }
    return bsStringNewSize(data, size);
}


/*
 * The string builder
 */


void bsSBInit(BSStringBuilder *sb)
{
    sb->data = NULL;
    sb->size = 0;
    sb->capacity = 0;
}


/* The buffer is the data of a string allocation, so bsSBToValue finishes it in place */
static BSString *bsSBString(const BSStringBuilder *sb)
{
    return sb->data != NULL ? (BSString *) (sb->data - BS_STRING_HEAP_HEAD - sizeof(BSString)) : NULL;
}

void bsSBFree(BSStringBuilder *sb)
{
    if (sb->data == bsTS.sbScratch) {
        bsTS.sbScratchBusy = false;
    } else {
        free(bsSBString(sb));
    }
    bsSBInit(sb);
}


/*
 * Make room for "size" more bytes. A builder starts in the thread's scratch buffer when it is
 * free, so a small result - most are - is finished as a pooled string with no allocation of its
 * own; one that outgrows the scratch, or starts while another builder holds it, has a heap block
 * that becomes the string uncopied, doubling as it fills or taking a larger request as it is.
 */
static BS_NOINLINE void bsSBGrow(BSStringBuilder *sb, size_t needed)
{
    if (sb->data == NULL && needed <= BS_SB_SCRATCH && !bsTS.sbScratchBusy) {
        bsTS.sbScratchBusy = true;
        sb->data = bsTS.sbScratch;
        sb->capacity = BS_SB_SCRATCH;
        return;
    }
    bool scratch = sb->data == bsTS.sbScratch;
    size_t capacity = sb->capacity != 0 ? sb->capacity * 2 : 32;
    if (capacity < needed) {
        capacity = needed;
    }
    BSString *string = bsRealloc(scratch ? NULL : bsSBString(sb), sizeof(BSString) + BS_STRING_HEAP_HEAD + capacity);
    char *data = BS_STRING_STORAGE(string) + BS_STRING_HEAP_HEAD;
    if (scratch) {
        memcpy(data, sb->data, sb->size);
        bsTS.sbScratchBusy = false;
    }
    sb->data = data;
    sb->capacity = capacity;
}


void bsSBReserve(BSStringBuilder *sb, size_t size)
{
    size_t needed = sb->size + size + 1;
    if (needed > sb->capacity) {
        bsSBGrow(sb, needed);
    }
}


void bsSBAppend(BSStringBuilder *sb, const char *text, size_t size)
{
    if (size == 0) {
        return;
    }
    bsSBReserve(sb, size);
    memcpy(sb->data + sb->size, text, size);
    sb->size += size;
    sb->data[sb->size] = '\0';
}


void bsSBAppendString(BSStringBuilder *sb, const char *text)
{
    bsSBAppend(sb, text, strlen(text));
}


void bsSBAppendChar(BSStringBuilder *sb, char ch)
{
    bsSBReserve(sb, 1);
    sb->data[sb->size++] = ch;
    sb->data[sb->size] = '\0';
}


void bsSBAppendFormat(BSStringBuilder *sb, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    BSValue text = bsStringNewVFormat(format, args);
    va_end(args);
    bsSBAppend(sb, bsStringData(text), bsStringSize(text));
    bsReleaseInline(text);
}


void bsSBAppendValue(BSStringBuilder *sb, BSValue value)
{
    char buffer[64];
    BSStringBytes bytes = bsStringBytes(value, buffer, sizeof(buffer));
    bsSBAppend(sb, bytes.data, bytes.size);
    bsReleaseInline(bytes.text);
}


/* Out of line: the pool allocation it inlines would bulk every builder's finish site */
BS_NOINLINE BSValue bsSBToValue(BSStringBuilder *sb)
{
    if (sb->data == NULL || sb->data == bsTS.sbScratch) {
        BSValue value = bsStringNewSize(sb->data != NULL ? sb->data : "", sb->size);
        bsSBFree(sb);
        return value;
    }
    BSString *string = bsSBString(sb);
    /* The buffer becomes the string, uncopied; built this way it is freed rather than pooled */
    bsStringInitHeap(string, sb->size, sb->capacity - 1);
    BSValue value = bsStringFinish(string, sb->size);
    bsSBInit(sb);
    return value;
}


/*
 * Array values
 */


#define BS_ARRAY_POOL_MAX 16384

/*
 * Recycle common array value buffers so JSON arrays are not two mallocs every time. A small array
 * born at its size - an array literal, an object's keys - is rounded up to a pool class, so a
 * two-element literal is a pool hit rather than a malloc and a free.
 */
#define BS_ARRAY_BUF_POOL_MAX 64

/* The pool class of a buffer capacity, or -1 for a capacity the pool does not hold */
static int bsArrayBufClassIndex(size_t capacity)
{
    switch (capacity) {
    case 2: return 0;
    case 4: return 1;
    case 8: return 2;
    case 16: return 3;
    case 32: return 4;
    case 64: return 5;
    default: return -1;
    }
}

/* A new array's buffer capacity: a small size rounded up to its pool class */
static size_t bsArrayBufCapacity(size_t capacity)
{
    return capacity == 0 || capacity >= 8 ? capacity : capacity <= 2 ? 2 : capacity <= 4 ? 4 : 8;
}

/* The pool paths stay out of line: inlined, they bulk every array-building site in the interpreter loop */
static BS_NOINLINE BSValue *bsArrayBufAlloc(size_t capacity)
{
    int classIndex = bsArrayBufClassIndex(capacity);
    if (classIndex >= 0 && bsTS.arrayBufPool[classIndex] != NULL) {
        BSValue *values = bsTS.arrayBufPool[classIndex];
        bsTS.arrayBufPool[classIndex] = (BSValue *) values[0].u.ref;
        bsTS.arrayBufPoolCount[classIndex]--;
        return values;
    }
    return bsAlloc(capacity * sizeof(BSValue));
}

static BS_NOINLINE void bsArrayBufFree(BSValue *values, size_t capacity)
{
    int classIndex = bsArrayBufClassIndex(capacity);
    if (classIndex >= 0) {
        BS_POOL_GIVE(bsTS.arrayBufPool[classIndex], bsTS.arrayBufPoolCount[classIndex], BS_ARRAY_BUF_POOL_MAX, values,
                     values[0].u.ref);
    } else {
        free(values);
    }
}

static BSArray *bsArrayAlloc(void)
{
    if (bsTS.arrayPool != NULL) {
        BSArray *array = bsTS.arrayPool;
        bsTS.arrayPool = (BSArray *) array->values;
        bsTS.arrayPoolCount--;
        return array;
    }
    return bsAlloc(sizeof(BSArray));
}

static void bsArrayRecycle(BSArray *array)
{
    BS_POOL_GIVE(bsTS.arrayPool, bsTS.arrayPoolCount, BS_ARRAY_POOL_MAX, array, array->values);
}


BSValue bsArrayNewCapacity(size_t capacity)
{
    BSArray *array = bsArrayAlloc();
    capacity = bsArrayBufCapacity(capacity);
    array->refcount = 1;
    array->count = 0;
    array->capacity = capacity;
    array->values = capacity != 0 ? bsArrayBufAlloc(capacity) : NULL;

    return (BSValue) {.type = BS_ARRAY, .u.array = array};
}


BSValue bsArrayNew(void)
{
    return bsArrayNewCapacity(0);
}


size_t bsArrayCount(BSValue value)
{
    return value.type == BS_ARRAY ? value.u.array->count : 0;
}


BSValue bsArrayGet(BSValue value, size_t index)
{
    if (value.type != BS_ARRAY || index >= value.u.array->count) {
        return bsNull();
    }
    return value.u.array->values[index];
}


void bsArrayReserve(BSValue value, size_t capacity)
{
    BSArray *array = value.u.array;
    if (capacity > array->capacity) {
        size_t newCapacity = array->capacity != 0 ? array->capacity : 8;
        while (newCapacity < capacity) {
            newCapacity *= 2;
        }
        BSValue *grown = bsArrayBufAlloc(newCapacity);
        if (array->values != NULL) {
            memcpy(grown, array->values, array->count * sizeof(BSValue));
            bsArrayBufFree(array->values, array->capacity);
        }
        array->values = grown;
        array->capacity = newCapacity;
    }
}


void bsArrayPush(BSValue value, BSValue item)
{
    BSArray *array = value.u.array;
    bsArrayReserve(value, array->count + 1);
    array->values[array->count++] = item;
}


void bsArraySet(BSValue value, size_t index, BSValue item)
{
    BSArray *array = value.u.array;
    bsReleaseInline(array->values[index]);
    array->values[index] = item;
}


void bsArrayInsert(BSValue value, size_t index, BSValue item)
{
    BSArray *array = value.u.array;
    bsArrayReserve(value, array->count + 1);
    memmove(array->values + index + 1, array->values + index, (array->count - index) * sizeof(BSValue));
    array->values[index] = item;
    array->count++;
}


void bsArrayDelete(BSValue value, size_t index)
{
    BSArray *array = value.u.array;
    bsReleaseInline(array->values[index]);
    memmove(array->values + index, array->values + index + 1, (array->count - index - 1) * sizeof(BSValue));
    array->count--;
}


BSValue bsArrayCopy(BSValue value)
{
    size_t count = bsArrayCount(value);
    BSValue copy = bsArrayNewCapacity(count);
    for (size_t ix = 0; ix < count; ix++) {
        bsArrayPush(copy, bsRetainInline(value.u.array->values[ix]));
    }
    return copy;
}


/* A stable merge sort - the reference implementations' sorts are stable */
static void bsArrayMergeSort(BSValue *values, BSValue *scratch, size_t count,
                             int (*compare)(BSValue, BSValue, void *), void *data)
{
    if (count < 2) {
        return;
    }
    size_t half = count / 2;
    bsArrayMergeSort(values, scratch, half, compare, data);
    bsArrayMergeSort(values + half, scratch + half, count - half, compare, data);

    size_t ixLeft = 0;
    size_t ixRight = half;
    size_t ixOut = 0;
    while (ixLeft < half && ixRight < count) {
        if (compare(values[ixRight], values[ixLeft], data) < 0) {
            scratch[ixOut++] = values[ixRight++];
        } else {
            scratch[ixOut++] = values[ixLeft++];
        }
    }
    while (ixLeft < half) {
        scratch[ixOut++] = values[ixLeft++];
    }
    while (ixRight < count) {
        scratch[ixOut++] = values[ixRight++];
    }
    memcpy(values, scratch, count * sizeof(BSValue));
}


void bsArraySort(BSValue value, int (*compare)(BSValue, BSValue, void *), void *data)
{
    size_t count = bsArrayCount(value);
    if (count < 2) {
        return;
    }
    BSValue *scratch = bsAlloc(count * sizeof(BSValue));
    bsArrayMergeSort(value.u.array->values, scratch, count, compare, data);
    free(scratch);
}


/*
 * Object values
 *
 * An object is an insertion-ordered array of key/value entries. Up to three entries live in the
 * object itself; past that they move to a heap buffer that doubles as it fills. An object of more
 * than sixteen keys also carries a hash index over its entries - an open-addressing table of
 * (hash, entry) slots keyed by the content hash the key string caches - so a lookup probes the
 * index and compares one key, while a smaller object scans its entries. Iteration is entry order,
 * which is insertion order (matching the reference implementations, whose objects are JavaScript
 * objects and Python dictionaries); the operations defined over sorted keys sort an index of the
 * entries on demand.
 *
 * A lookup compares an entry's key to the key sought by pointer first - interned names, which
 * compiled code and the library use, hit that way - and by content only when the two are not both
 * interned, since two distinct interned strings never have the same content.
 */


#define BS_OBJECT_POOL_MAX 16384

/* Entries scanned in place before an object builds its hash index */
#define BS_OBJECT_LINEAR 16

/* generation's high bit: a non-interned key has been stored, so an interned miss must scan by content */
#define BS_OBJECT_ORDINARY 0x80000000u

/* Recycled entry buffers, in the two capacities an object outgrows its inline entries into first */
#define BS_ENTRY_POOL_CAPACITY 8
#define BS_ENTRY_POOL_MAX 1024


static BSObject *bsObjectAlloc(void)
{
    if (bsTS.objectPool != NULL) {
        BSObject *object = bsTS.objectPool;
        bsTS.objectPool = (BSObject *) object->entries;
        bsTS.objectPoolCount--;
        return object;
    }
    return bsAlloc(sizeof(BSObject));
}

static void bsObjectRecycle(BSObject *object)
{
    BS_POOL_GIVE(bsTS.objectPool, bsTS.objectPoolCount, BS_OBJECT_POOL_MAX, object, object->entries);
}


BSValue bsObjectNew(void)
{
    BSObject *object = bsObjectAlloc();
    object->refcount = 1;
    object->count = 0;
    object->capacity = BS_OBJECT_INLINE;
    object->generation = 0;
    object->entries = object->inline_;
    object->index = NULL;

    return (BSValue) {.type = BS_OBJECT, .u.object = object};
}


size_t bsObjectCount(BSValue value)
{
    return value.type == BS_OBJECT ? value.u.object->count : 0;
}


/* The pool class of an entry buffer capacity, or -1 for a capacity the pool does not hold */
static int bsEntryPoolClass(size_t capacity)
{
    return capacity == BS_ENTRY_POOL_CAPACITY ? 0 : (capacity == 2 * BS_ENTRY_POOL_CAPACITY ? 1 : -1);
}

static BSObjectEntry *bsEntriesAlloc(size_t capacity)
{
    int classIndex = bsEntryPoolClass(capacity);
    if (classIndex >= 0 && bsTS.entryPool[classIndex] != NULL) {
        BSObjectEntry *entries = bsTS.entryPool[classIndex];
        bsTS.entryPool[classIndex] = (BSObjectEntry *) entries[0].key;
        bsTS.entryPoolCount[classIndex]--;
        return entries;
    }
    return bsAlloc(capacity * sizeof(BSObjectEntry));
}

static void bsEntriesFree(BSObjectEntry *entries, size_t capacity)
{
    int classIndex = bsEntryPoolClass(capacity);
    if (classIndex >= 0) {
        BS_POOL_GIVE(bsTS.entryPool[classIndex], bsTS.entryPoolCount[classIndex], BS_ENTRY_POOL_MAX, entries, entries[0].key);
    } else {
        free(entries);
    }
}


static int bsKeyCompare(const BSString *key1, const BSString *key2)
{
    if (key1 == key2) {
        return 0;
    }
    size_t size1 = key1->size;
    size_t size2 = key2->size;
    size_t size = size1 < size2 ? size1 : size2;
    int result = size != 0 ? memcmp(key1->data, key2->data, size) : 0;
    if (result != 0) {
        return result;
    }
    return size1 < size2 ? -1 : (size1 == size2 ? 0 : 1);
}


/*
 * Whether a stored key is the key sought: "key" is the sought key's string when the lookup has
 * one, or NULL for a C-string lookup, and "data" and "size" are its bytes either way
 */
static inline bool bsKeyEqual(const BSString *stored, const BSString *key, const char *data, size_t size)
{
    if (stored == key) {
        return true;
    }
    if (key != NULL && (stored->flags & key->flags & BS_STR_INTERNED) != 0) {
        return false;
    }
    return stored->size == size && (size == 0 || memcmp(stored->data, data, size) == 0);
}


/*
 * The hash index
 */


static void bsObjectIndexPut(BSObjectIndex *index, uint32_t hash, uint32_t entry)
{
    for (uint32_t probe = hash;; probe++) {
        BSObjectSlot *slot = &index->slots[probe & index->mask];
        if (slot->entry == 0) {
            slot->hash = hash;
            slot->entry = entry + 1;
            return;
        }
    }
}


/* Build - or rebuild - the index over the entries, sized for the object's capacity at half load */
static BS_NOINLINE void bsObjectIndexBuild(BSObject *object)
{
    uint32_t size = 32;
    while (size < object->capacity * 2) {
        size *= 2;
    }
    free(object->index);
    object->index = bsAlloc(sizeof(BSObjectIndex) + size * sizeof(BSObjectSlot));
    object->index->mask = size - 1;
    memset(object->index->slots, 0, size * sizeof(BSObjectSlot));
    for (uint32_t ix = 0; ix < object->count; ix++) {
        bsObjectIndexPut(object->index, bsStringHash(object->entries[ix].key), ix);
    }
}


/*
 * The entry holding a key, or NULL - "key" is the sought key's string, or NULL for a C string
 *
 * A scan of the entries compares pointers first, over every entry, and content only on a miss:
 * the keys compiled code and the library look up are interned, as are the keys they store, so the
 * pointer pass finds them without touching a stored key's header. A sought key that is interned
 * can then only match a stored key that is not - two distinct interned strings never compare
 * equal - so the content pass skips the interned ones, and is skipped entirely until a
 * non-interned key has been stored (generation's BS_OBJECT_ORDINARY bit).
 */
static inline BSObjectEntry *bsObjectFind(const BSObject *object, BSString *key, const char *data, size_t size)
{
    BSObjectEntry *entries = object->entries;
    const BSObjectIndex *index = object->index;
    if (index == NULL) {
        size_t count = object->count;
        if (key != NULL) {
            for (size_t ix = 0; ix < count; ix++) {
                if (entries[ix].key == key) {
                    return &entries[ix];
                }
            }
            if ((key->flags & BS_STR_INTERNED) != 0) {
                if ((object->generation & BS_OBJECT_ORDINARY) == 0) {
                    return NULL;
                }
                for (size_t ix = 0; ix < count; ix++) {
                    const BSString *stored = entries[ix].key;
                    if ((stored->flags & BS_STR_INTERNED) == 0 && stored->size == size &&
                        memcmp(stored->data, data, size) == 0) {
                        return &entries[ix];
                    }
                }
                return NULL;
            }
        }
        for (size_t ix = 0; ix < count; ix++) {
            const BSString *stored = entries[ix].key;
            if (stored->size == size && memcmp(stored->data, data, size) == 0) {
                return &entries[ix];
            }
        }
        return NULL;
    }
    uint32_t hash = key != NULL ? bsStringHash(key) : bsHashBytes(data, size);
    for (uint32_t probe = hash;; probe++) {
        const BSObjectSlot *slot = &index->slots[probe & index->mask];
        if (slot->entry == 0) {
            return NULL;
        }
        if (slot->hash == hash && bsKeyEqual(entries[slot->entry - 1].key, key, data, size)) {
            return &entries[slot->entry - 1];
        }
    }
}


/* Double a full object's entry buffer - and its index, whose slot count follows the capacity */
static BS_NOINLINE void bsObjectEntriesGrow(BSObject *object)
{
    size_t capacity = object->capacity < BS_ENTRY_POOL_CAPACITY ? BS_ENTRY_POOL_CAPACITY : object->capacity * 2;
    BSObjectEntry *entries = bsEntriesAlloc(capacity);
    memcpy(entries, object->entries, object->count * sizeof(BSObjectEntry));
    if (object->entries != object->inline_) {
        bsEntriesFree(object->entries, object->capacity);
    }
    object->entries = entries;
    object->capacity = (uint32_t) capacity;
    if (object->index != NULL) {
        bsObjectIndexBuild(object);
    }
}


/* Append an entry for a key known to be absent. Takes ownership of "item" and retains "key". */
static void bsObjectEntryAdd(BSObject *object, BSValue key, BSValue item)
{
    if (object->count == object->capacity) {
        bsObjectEntriesGrow(object);
    }
    uint32_t ix = object->count++;
    BSObjectEntry *entry = &object->entries[ix];
    entry->key = bsRetainInline(key).u.string;
    entry->value = item;
    object->generation++;
    if ((entry->key->flags & BS_STR_INTERNED) == 0) {
        object->generation |= BS_OBJECT_ORDINARY;
    }
    if (object->index != NULL) {
        bsObjectIndexPut(object->index, bsStringHash(entry->key), ix);
    } else if (object->count > BS_OBJECT_LINEAR) {
        bsObjectIndexBuild(object);
    }
}


BSValue bsObjectNewCapacity(size_t count)
{
    BSValue value = bsObjectNew();
    BSObject *object = value.u.object;
    if (count > BS_OBJECT_INLINE) {
        /* Born with entries for every key, and past the scan threshold its index, so appends never rebuild */
        size_t capacity = BS_ENTRY_POOL_CAPACITY;
        while (capacity < count) {
            capacity *= 2;
        }
        object->entries = bsEntriesAlloc(capacity);
        object->capacity = (uint32_t) capacity;
        if (count > BS_OBJECT_LINEAR) {
            bsObjectIndexBuild(object);
        }
    }
    return value;
}


/* Release an object's entries and their buffer */
static void bsObjectEntriesFree(BSObject *object)
{
    BSObjectEntry *entries = object->entries;
    for (size_t ix = 0; ix < object->count; ix++) {
        bsReleaseInline(bsStringTake(entries[ix].key));
        bsReleaseInline(entries[ix].value);
    }
    if (entries != object->inline_) {
        bsEntriesFree(entries, object->capacity);
    }
    if (object->index != NULL) {
        free(object->index);
    }
}


void bsObjectAppend(BSValue value, BSValue key, BSValue item)
{
    bsObjectEntryAdd(value.u.object, key, item);
}


/* Insert or update a key. Takes ownership of "item"; retains "key" if an entry is added. */
static void bsObjectInsert(BSObject *object, BSValue key, BSValue item)
{
    BSObjectEntry *entry = bsObjectFind(object, key.u.string, key.u.string->data, key.u.string->size);
    if (entry != NULL) {
        bsReleaseInline(entry->value);
        entry->value = item;
        return;
    }
    bsObjectEntryAdd(object, key, item);
}


void bsObjectSetString(BSValue value, BSValue key, BSValue item)
{
    bsObjectInsert(value.u.object, key, item);
}


void bsObjectSet(BSValue value, const char *key, BSValue item)
{
    BSValue keyValue = bsStringIntern(key, strlen(key));
    bsObjectInsert(value.u.object, keyValue, item);
    bsReleaseInline(keyValue);
}


BSValue *bsObjectValuePtr(BSValue object, const char *key, size_t size)
{
    BSObjectEntry *entry = bsObjectFind(object.u.object, NULL, key, size);
    return entry != NULL ? &entry->value : NULL;
}


BSValue *bsObjectValuePtrString(BSValue object, BSValue key)
{
    BSObjectEntry *entry = bsObjectFind(object.u.object, key.u.string, key.u.string->data, key.u.string->size);
    return entry != NULL ? &entry->value : NULL;
}


BSObjectEntry *bsObjectEntryFind(BSObject *object, BSString *key)
{
    return bsObjectFind(object, key, key->data, key->size);
}


bool bsObjectLookupString(BSValue object, BSValue key, BSValue *out)
{
    BSValue *found = object.type == BS_OBJECT ? bsObjectValuePtrString(object, key) : NULL;
    if (found == NULL) {
        return false;
    }
    *out = *found;
    return true;
}


BSValue bsObjectGetString(BSValue value, BSValue key)
{
    BSValue *found = value.type == BS_OBJECT ? bsObjectValuePtrString(value, key) : NULL;
    return found != NULL ? *found : bsNull();
}


bool bsObjectKeyIs(const BSString *stored, BSValue key)
{
    return bsKeyEqual(stored, key.u.string, key.u.string->data, key.u.string->size);
}


BSValue bsObjectGet(BSValue value, const char *key)
{
    BSValue *found = value.type == BS_OBJECT ? bsObjectValuePtr(value, key, strlen(key)) : NULL;
    return found != NULL ? *found : bsNull();
}


bool bsObjectHasString(BSValue value, BSValue key)
{
    return value.type == BS_OBJECT && bsObjectValuePtrString(value, key) != NULL;
}


bool bsObjectHas(BSValue value, const char *key)
{
    return value.type == BS_OBJECT && bsObjectValuePtr(value, key, strlen(key)) != NULL;
}


bool bsObjectDelete(BSValue value, const char *key)
{
    BSObject *object = value.u.object;
    BSObjectEntry *entry = bsObjectFind(object, NULL, key, strlen(key));
    if (entry == NULL) {
        return false;
    }
    bsReleaseInline(bsStringTake(entry->key));
    bsReleaseInline(entry->value);
    size_t ix = (size_t) (entry - object->entries);
    object->count--;
    memmove(entry, entry + 1, (object->count - ix) * sizeof(BSObjectEntry));
    object->generation++;
    if (object->index != NULL) {
        /* The entries after it moved down, so the index is rebuilt - or dropped, below the threshold */
        if (object->count > BS_OBJECT_LINEAR) {
            bsObjectIndexBuild(object);
        } else {
            free(object->index);
            object->index = NULL;
        }
    }
    return true;
}


/* Sort entry indexes by key: an insertion sort of a short run, a merge sort above that */
static BS_NOINLINE void bsObjectSortIndexes(const BSObjectEntry *entries, uint32_t *order, uint32_t *scratch, size_t count)
{
    if (count <= 16) {
        for (size_t i = 1; i < count; i++) {
            uint32_t item = order[i];
            const BSString *key = entries[item].key;
            size_t j = i;
            while (j > 0 && bsKeyCompare(entries[order[j - 1]].key, key) > 0) {
                order[j] = order[j - 1];
                j--;
            }
            order[j] = item;
        }
        return;
    }
    size_t half = count / 2;
    bsObjectSortIndexes(entries, order, scratch, half);
    bsObjectSortIndexes(entries, order + half, scratch + half, count - half);
    size_t ixLeft = 0;
    size_t ixRight = half;
    size_t ixOut = 0;
    while (ixLeft < half && ixRight < count) {
        if (bsKeyCompare(entries[order[ixLeft]].key, entries[order[ixRight]].key) <= 0) {
            scratch[ixOut++] = order[ixLeft++];
        } else {
            scratch[ixOut++] = order[ixRight++];
        }
    }
    while (ixLeft < half) {
        scratch[ixOut++] = order[ixLeft++];
    }
    while (ixRight < count) {
        scratch[ixOut++] = order[ixRight++];
    }
    memcpy(order, scratch, count * sizeof(uint32_t));
}


bool bsObjectIterSorted(BSValue value, BSObjectIterFn iter, void *data)
{
    if (value.type != BS_OBJECT) {
        return true;
    }
    const BSObject *object = value.u.object;
    size_t count = object->count;
    uint32_t orderInline[2 * 32];
    uint32_t *order = count <= 32 ? orderInline : bsAlloc(2 * count * sizeof(uint32_t));
    for (uint32_t ix = 0; ix < count; ix++) {
        order[ix] = ix;
    }
    bsObjectSortIndexes(object->entries, order, order + count, count);
    bool complete = true;
    for (size_t ix = 0; ix < count && complete; ix++) {
        const BSObjectEntry *entry = &object->entries[order[ix]];
        complete = iter(bsStringTake(entry->key), entry->value, data);
    }
    if (order != orderInline) {
        free(order);
    }
    return complete;
}


bool bsObjectIter(BSValue value, BSObjectIterFn iter, void *data)
{
    if (value.type != BS_OBJECT) {
        return true;
    }
    const BSObject *object = value.u.object;
    for (size_t ix = 0; ix < object->count; ix++) {
        const BSObjectEntry *entry = &object->entries[ix];
        if (!iter(bsStringTake(entry->key), entry->value, data)) {
            return false;
        }
    }
    return true;
}


static bool bsObjectKeysIter(BSValue key, BSValue item, void *data)
{
    bsArrayPush(*((BSValue *) data), bsRetainInline(key));
    return true;
}


BSValue bsObjectKeys(BSValue value)
{
    size_t count = bsObjectCount(value);
    BSValue keys = bsArrayNewCapacity(count);
    BSArray *array = keys.u.array;
    for (size_t ix = 0; ix < count; ix++) {
        array->values[ix] = bsRetainInline(bsStringTake(value.u.object->entries[ix].key));
    }
    array->count = count;
    return keys;
}


BSValue bsObjectKeysSorted(BSValue value)
{
    BSValue keys = bsArrayNewCapacity(bsObjectCount(value));
    bsObjectIterSorted(value, bsObjectKeysIter, &keys);
    return keys;
}


void bsObjectAssign(BSValue dest, BSValue src)
{
    const BSObject *source = src.u.object;
    for (size_t ix = 0; ix < source->count; ix++) {
        const BSObjectEntry *entry = &source->entries[ix];
        bsObjectInsert(dest.u.object, bsStringTake(entry->key), bsRetainInline(entry->value));
    }
}


BSValue bsObjectCopy(BSValue value)
{
    size_t count = bsObjectCount(value);
    BSValue copy = bsObjectNewCapacity(count);
    for (size_t ix = 0; ix < count; ix++) {
        const BSObjectEntry *entry = &value.u.object->entries[ix];
        bsObjectEntryAdd(copy.u.object, bsStringTake(entry->key), bsRetainInline(entry->value));
    }
    return copy;
}


/*
 * Function values
 */


BSValue bsFunctionNew(const char *name, BSFunctionFn fn, void *data, void (*dataFree)(void *data))
{
    BSFunction *function = bsAlloc(sizeof(BSFunction));
    function->refcount = 1;
    function->fn = fn;
    function->data = data;
    function->dataFree = dataFree;
    function->intrinsic = BS_INTRIN_NONE;

    return (BSValue) {.type = BS_FUNCTION, .u.function = function};
}


BSValue bsFunctionCall(BSValue function, const BSValue *args, size_t argCount, BSOptions *options)
{
    if (function.type != BS_FUNCTION) {
        return bsNull();
    }
    BSFunction *fn = function.u.function;
    return fn->fn(args, argCount, options, fn->data);
}


/*
 * Reference counting
 */


BSValue bsRetain(BSValue value)
{
    return bsRetainInline(value);
}


void bsReleaseDestroyed(BSValue value)
{
    switch (value.type) {
    case BS_STRING:
        bsStringFree(value.u.string);
        break;
    case BS_ARRAY: {
        BSArray *array = value.u.array;
        for (size_t ix = 0; ix < array->count; ix++) {
            bsReleaseInline(array->values[ix]);
        }
        if (array->values != NULL) {
            bsArrayBufFree(array->values, array->capacity);
        }
        bsArrayRecycle(array);
        break;
    }
    case BS_OBJECT: {
        BSObject *object = value.u.object;
        bsObjectEntriesFree(object);
        bsObjectRecycle(object);
        break;
    }
    case BS_REGEX:
        bsRegexDestroy(value);
        break;
    default: {
        BSFunction *function = value.u.function;
        if (function->dataFree != NULL) {
            function->dataFree(function->data);
        }
        free(function);
        break;
    }
    }
}


void bsRelease(BSValue value)
{
    bsReleaseInline(value);
}


void bsAssign(BSValue *target, BSValue value)
{
    bsAssignInline(target, value);
}


void bsValueCleanup(void)
{
    bsRegexScratchFree();

    /* The intern table's references - a string still held elsewhere lives on as an ordinary string */
    if (bsTS.internSlots != NULL) {
        for (size_t ix = 0; ix <= bsTS.internMask; ix++) {
            BSString *string = bsTS.internSlots[ix].string;
            if (string != NULL) {
                string->flags &= (uint8_t) ~BS_STR_INTERNED;
                bsReleaseInline(bsStringTake(string));
            }
        }
        free(bsTS.internSlots);
        bsTS.internSlots = NULL;
        bsTS.internMask = 0;
        bsTS.internCount = 0;
    }

    /* The shared short strings - one still held elsewhere lives on as an ordinary string */
    for (size_t ix = 0; ix < BS_COUNT_OF(bsTS.shortStrings); ix++) {
        if (bsTS.shortStrings[ix] != NULL) {
            bsReleaseInline(bsStringTake(bsTS.shortStrings[ix]));
            bsTS.shortStrings[ix] = NULL;
        }
    }

    /* The free lists */
    for (unsigned ix = 0; ix < BS_STRING_POOL_CLASSES; ix++) {
        BS_POOL_DRAIN(bsTS.stringPool[ix], bsTS.stringPoolCount[ix], BSString, item->index);
    }
    BS_POOL_DRAIN(bsTS.arrayPool, bsTS.arrayPoolCount, BSArray, item->values);
    for (int ix = 0; ix < BS_ARRAY_BUF_CLASS_COUNT; ix++) {
        BS_POOL_DRAIN(bsTS.arrayBufPool[ix], bsTS.arrayBufPoolCount[ix], BSValue, item[0].u.ref);
    }
    BS_POOL_DRAIN(bsTS.objectPool, bsTS.objectPoolCount, BSObject, item->entries);
    for (int ix = 0; ix < BS_ENTRY_POOL_CLASS_COUNT; ix++) {
        BS_POOL_DRAIN(bsTS.entryPool[ix], bsTS.entryPoolCount[ix], BSObjectEntry, item[0].key);
    }
}


/*
 * Datetime values
 */


static int64_t bsFloorDiv(int64_t value, int64_t divisor)
{
    int64_t quotient = value / divisor;
    if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) {
        quotient--;
    }
    return quotient;
}


/* Days since the Unix epoch for a civil date - Howard Hinnant's days_from_civil */
static int64_t bsDaysFromCivil(int64_t year, int64_t month, int64_t day)
{
    year -= (month <= 2 ? 1 : 0);
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    int64_t yearOfEra = year - era * 400;
    int64_t dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + dayOfEra - 719468;
}


/* The local time offset, in seconds east of UTC, at a UTC instant */
static int64_t bsLocalOffset(int64_t utcSeconds)
{
    time_t clock = (time_t) utcSeconds;
    struct tm local;
    if (localtime_r(&clock, &local) == NULL) {
        return 0; /* GCOV_EXCL_LINE */
    }
    int64_t civil = bsDaysFromCivil(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday) * 86400 +
        local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    return civil - utcSeconds;
}


void bsDatetimeParts(int64_t milliseconds, BSDatetimeParts *parts)
{
    int64_t seconds = bsFloorDiv(milliseconds, 1000);
    int millisecond = (int) (milliseconds - seconds * 1000);
    time_t clock = (time_t) seconds;
    struct tm local;
    /* GCOV_EXCL_START */
    if (localtime_r(&clock, &local) == NULL) {
        memset(parts, 0, sizeof(*parts));
        parts->year = 1970;
        parts->month = 1;
        parts->day = 1;
        return;
    }
    /* GCOV_EXCL_STOP */
    parts->year = local.tm_year + 1900;
    parts->month = local.tm_mon + 1;
    parts->day = local.tm_mday;
    parts->hour = local.tm_hour;
    parts->minute = local.tm_min;
    parts->second = local.tm_sec;
    parts->millisecond = millisecond;
    parts->tzOffset = (int) (-bsLocalOffset(seconds) / 60);
}


bool bsDatetimeFromParts(double year, double month, double day, double hour, double minute,
                         double second, double millisecond, int64_t *result)
{
    /* The year and month-index limits are V8's - past them a huge component could cancel another */
    double monthIndex = month - 1;
    if (!(fabs(year) <= 1000000 && fabs(monthIndex) <= 10000000)) {
        return false;
    }

    /* Roll the month into the year, then sum the components as doubles, which a huge one cannot overflow */
    double extraYears = floor(monthIndex / 12);
    int64_t monthDays = bsDaysFromCivil((int64_t) (year + extraYears), (int64_t) (monthIndex - extraYears * 12) + 1, 1);
    double time = hour * 3600000 + minute * 60000 + second * 1000 + millisecond;
    double local = ((double) monthDays + day - 1) * 86400000 + time;
    if (!(fabs(local) <= BS_DATETIME_MAX + 86400000)) {
        return false;
    }

    /* Local time to UTC, resolving the offset at the UTC instant, then JavaScript's TimeClip */
    int64_t localMs = (int64_t) local;
    int64_t civil = bsFloorDiv(localMs, 1000);
    int64_t utc = civil - bsLocalOffset(civil);
    utc = civil - bsLocalOffset(utc);
    int64_t utcMs = utc * 1000 + (localMs - civil * 1000);
    if (utcMs < -(int64_t) BS_DATETIME_MAX || utcMs > (int64_t) BS_DATETIME_MAX) {
        return false;
    }
    *result = utcMs;
    return true;
}


int64_t bsDatetimeNow(void)
{
    struct timespec now;
    /* GCOV_EXCL_START */
    if (timespec_get(&now, TIME_UTC) != TIME_UTC) {
        return (int64_t) time(NULL) * 1000;
    }
    /* GCOV_EXCL_STOP */
    return (int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}


int64_t bsDatetimeToday(void)
{
    BSDatetimeParts parts;
    bsDatetimeParts(bsDatetimeNow(), &parts);
    int64_t midnight = 0;
    bsDatetimeFromParts(parts.year, parts.month, parts.day, 0, 0, 0, 0, &midnight); /* today is in range */
    return midnight;
}


static bool bsParseDigits(const char *text, size_t offset, size_t count, int *result)
{
    int value = 0;
    for (size_t ix = 0; ix < count; ix++) {
        char ch = text[offset + ix];
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10 + (ch - '0');
    }
    *result = value;
    return true;
}


bool bsDatetimeParse(const char *text, size_t size, int64_t *result)
{
    /* The date - "YYYY-MM-DD" - alone is a local-time date; the datetime form continues with
       "THH:MM:SS[.fff](Z|(+|-)HH:MM)" */
    int year, month, day;
    if ((size != 10 && size < 20) || text[4] != '-' || text[7] != '-' ||
        !bsParseDigits(text, 0, 4, &year) || !bsParseDigits(text, 5, 2, &month) ||
        !bsParseDigits(text, 8, 2, &day) || month < 1 || month > 12 || day < 1 || day > 31) {
        return false;
    }
    if (size == 10) {
        return bsDatetimeFromParts(year, month, day, 0, 0, 0, 0, result);
    }
    int hour, minute, second;
    if (text[10] != 'T' || text[13] != ':' || text[16] != ':' ||
        !bsParseDigits(text, 11, 2, &hour) || !bsParseDigits(text, 14, 2, &minute) ||
        !bsParseDigits(text, 17, 2, &second) || hour > 24 || minute > 59 || second > 59) {
        return false;
    }

    /* The optional fractional second */
    size_t ix = 19;
    int millisecond = 0;
    if (ix < size && text[ix] == '.') {
        ix++;
        size_t fractionBegin = ix;
        while (ix < size && text[ix] >= '0' && text[ix] <= '9') {
            ix++;
        }
        size_t fractionSize = ix - fractionBegin;
        if (fractionSize < 1 || fractionSize > 6) {
            return false;
        }
        int scale[3] = {100, 10, 1};
        for (size_t ixFraction = 0; ixFraction < fractionSize && ixFraction < 3; ixFraction++) {
            millisecond += (text[fractionBegin + ixFraction] - '0') * scale[ixFraction];
        }
    }

    /* The time zone - "Z" or an offset */
    int64_t tzOffsetSeconds;
    if (ix < size && text[ix] == 'Z' && ix + 1 == size) {
        tzOffsetSeconds = 0;
    } else if (ix + 6 == size && (text[ix] == '+' || text[ix] == '-') && text[ix + 3] == ':') {
        int tzHour, tzMinute;
        if (!bsParseDigits(text, ix + 1, 2, &tzHour) || !bsParseDigits(text, ix + 4, 2, &tzMinute)) {
            return false;
        }
        if (tzHour > 23 || tzMinute > 59) {
            return false;
        }
        tzOffsetSeconds = (int64_t) tzHour * 3600 + tzMinute * 60;
        if (text[ix] == '-') {
            tzOffsetSeconds = -tzOffsetSeconds;
        }
    } else {
        return false;
    }

    /* ISO 8601's hour-24 end-of-day form is only valid at exactly midnight */
    int extraDay = 0;
    if (hour == 24) {
        if (minute != 0 || second != 0 || millisecond != 0) {
            return false;
        }
        hour = 0;
        extraDay = 1;
    }

    int64_t civil = bsDaysFromCivil(year, month, day + extraDay) * 86400 +
        (int64_t) hour * 3600 + (int64_t) minute * 60 + second;
    *result = (civil - tzOffsetSeconds) * 1000 + millisecond;
    return true;
}


/*
 * Value accessors
 */


const char *const bsTypeNames[BS_REGEX + 1] = {
    "null", "boolean", "number", "datetime", "string", "array", "object", "function", "regex"
};


const char *bsValueTypeString(BSValue value)
{
    return bsTypeNames[value.type];
}


BSValue bsValueString(BSValue value)
{
    char buffer[64];
    switch (value.type) {
    case BS_NULL:
    case BS_BOOLEAN:
    case BS_NUMBER: {
        BSStringBytes bytes = bsStringBytes(value, buffer, sizeof(buffer));
        return bsStringNewAscii(bytes.data, bytes.size);
    }
    case BS_DATETIME: {
        BSDatetimeParts parts;
        bsDatetimeParts(value.u.datetime, &parts);
        char tzSign = (parts.tzOffset <= 0 ? '+' : '-');
        int tzAbs = (parts.tzOffset < 0 ? -parts.tzOffset : parts.tzOffset);
        if (parts.millisecond == 0) {
            return bsStringNewFormat("%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
                                     parts.year, parts.month, parts.day, parts.hour, parts.minute,
                                     parts.second, tzSign, tzAbs / 60, tzAbs % 60);
        }
        return bsStringNewFormat("%04d-%02d-%02dT%02d:%02d:%02d.%03d%c%02d:%02d",
                                 parts.year, parts.month, parts.day, parts.hour, parts.minute,
                                 parts.second, parts.millisecond, tzSign, tzAbs / 60, tzAbs % 60);
    }
    case BS_STRING:
        return bsRetainInline(value);
    case BS_ARRAY:
    case BS_OBJECT:
        return bsJSONEncode(value, 0);
    case BS_FUNCTION:
        return bsStringNew("<function>");
    default:
        return bsStringNew("<regex>");
    }
}


bool bsValueBoolean(BSValue value)
{
    switch (value.type) {
    case BS_NULL:
        return false;
    case BS_BOOLEAN:
        return value.u.boolean;
    case BS_NUMBER:
        return value.u.number != 0;
    case BS_STRING:
        return value.u.string->size != 0;
    case BS_ARRAY:
        return value.u.array->count != 0;
    default:
        return true;
    }
}


bool bsValueIs(BSValue value1, BSValue value2)
{
    if (value1.type != value2.type) {
        return false;
    }
    switch (value1.type) {
    case BS_NULL:
        return true;
    case BS_BOOLEAN:
        return value1.u.boolean == value2.u.boolean;
    case BS_NUMBER:
        return value1.u.number == value2.u.number;
    case BS_DATETIME:
        return value1.u.datetime == value2.u.datetime;
    default:
        return value1.u.ref == value2.u.ref;
    }
}


/* The three-way comparison of two ordered operands; an unordered pair (a NaN) compares greater */
#define BS_COMPARE(left, right) ((left) < (right) ? -1 : ((left) == (right) ? 0 : 1))


int bsValueCompare(BSValue left, BSValue right)
{
    /* Null orders before every other type; otherwise values of different types compare by type name */
    if (left.type != right.type) {
        if (left.type == BS_NULL || right.type == BS_NULL) {
            return left.type == BS_NULL ? -1 : 1;
        }
        return strcmp(bsTypeNames[left.type], bsTypeNames[right.type]) < 0 ? -1 : 1;
    }
    switch (left.type) {
    case BS_NULL:
        return 0;
    case BS_STRING: {
        int compare = bsKeyCompare(left.u.string, right.u.string);
        return BS_COMPARE(compare, 0);
    }
    case BS_BOOLEAN:
        return BS_COMPARE(left.u.boolean, right.u.boolean);
    case BS_NUMBER:
        return BS_COMPARE(left.u.number, right.u.number);
    case BS_DATETIME:
        return BS_COMPARE(left.u.datetime, right.u.datetime);
    case BS_ARRAY: {
        size_t leftCount = left.u.array->count;
        size_t rightCount = right.u.array->count;
        size_t count = leftCount < rightCount ? leftCount : rightCount;
        for (size_t ix = 0; ix < count; ix++) {
            int compare = bsValueCompare(left.u.array->values[ix], right.u.array->values[ix]);
            if (compare != 0) {
                return compare;
            }
        }
        return BS_COMPARE(leftCount, rightCount);
    }
    case BS_OBJECT: {
        BSValue leftKeys = bsObjectKeysSorted(left);
        BSValue rightKeys = bsObjectKeysSorted(right);
        size_t leftCount = bsArrayCount(leftKeys);
        size_t rightCount = bsArrayCount(rightKeys);
        size_t count = leftCount < rightCount ? leftCount : rightCount;
        int result = 0;
        for (size_t ix = 0; ix < count && result == 0; ix++) {
            BSValue leftKey = bsArrayGet(leftKeys, ix);
            BSValue rightKey = bsArrayGet(rightKeys, ix);
            result = bsValueCompare(leftKey, rightKey);
            if (result == 0) {
                result = bsValueCompare(bsObjectGetString(left, leftKey), bsObjectGetString(right, rightKey));
            }
        }
        bsReleaseInline(leftKeys);
        bsReleaseInline(rightKeys);
        if (result != 0) {
            return result;
        }
        return BS_COMPARE(leftCount, rightCount);
    }
    default:
        /* Two functions or two regexes are unordered, like values of different types of one name */
        return 1;
    }
}
