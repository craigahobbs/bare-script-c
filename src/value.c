/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The BareScript value system
 */

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "barescript/json.h"
#include "barescript/regex.h"
#include "barescript/runtime.h"
#include "barescript/value.h"

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


void *bsAlloc(size_t size)
{
    void *ptr = malloc(size);
    /* GCOV_EXCL_START */
    if (ptr == NULL) {
        bsOutOfMemory();
    }
    /* GCOV_EXCL_STOP */
    return ptr;
}


void *bsRealloc(void *ptr, size_t size)
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
 * Value constructors
 */


BSValue bsNull(void)
{
    BSValue value;
    value.type = BS_NULL;
    value.u.ref = NULL;
    return value;
}


BSValue bsBoolean(bool boolean)
{
    BSValue value;
    value.type = BS_BOOLEAN;
    value.u.ref = NULL;
    value.u.boolean = boolean;
    return value;
}


BSValue bsNumber(double number)
{
    BSValue value;
    value.type = BS_NUMBER;
    value.u.number = number;
    return value;
}


BSValue bsDatetime(int64_t milliseconds)
{
    BSValue value;
    value.type = BS_DATETIME;
    value.u.datetime = milliseconds;
    return value;
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
 * String values
 */


static BSString *bsStringAlloc(size_t size)
{
    BSString *string = bsAlloc(sizeof(BSString) + size);
    string->refcount = 1;
    string->interned = 0;
    string->size = size;
    string->length = 0;
    string->offsets = NULL;
    string->cursorIndex = 0;
    string->cursorOffset = 0;
    string->data[size] = '\0';
    return string;
}


static bool bsUtf8IsAscii(const char *data, size_t size)
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


BSValue bsStringNewAscii(const char *text, size_t size)
{
    BSString *string = bsStringAlloc(size);
    memcpy(string->data, text, size);
    string->length = size;
    return bsStringTake(string);
}


BSValue bsStringNewSize(const char *text, size_t size)
{
    if (bsUtf8IsAscii(text, size)) {
        return bsStringNewAscii(text, size);
    }
    BSString *string = bsStringAlloc(size);
    memcpy(string->data, text, size);
    size_t length = bsUTF8Length(string->data, size);
    string->length = (length != SIZE_MAX ? length : size);
    return bsStringTake(string);
}


BSValue bsStringNew(const char *text)
{
    return bsStringNewSize(text, strlen(text));
}


BSValue bsStringTake(BSString *string)
{
    BSValue value;
    value.type = BS_STRING;
    value.u.string = string;
    return value;
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
    if (bsUtf8IsAscii(string->data, (size_t) size)) {
        string->length = (size_t) size;
        return bsStringTake(string);
    }
    size_t length = bsUTF8Length(string->data, (size_t) size);
    string->length = (length != SIZE_MAX ? length : (size_t) size);
    return bsStringTake(string);
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
 * Resolve a value to its string bytes, formatting scalars into a caller-supplied buffer
 *
 * Returns an owned string value for the types that need one - a null value when the bytes point at
 * the caller's buffer or at an existing string.
 */
static BSValue bsStringBytes(BSValue value, char *buffer, size_t bufferSize, const char **data,
                             size_t *size, size_t *length)
{
    switch (value.type) {
    case BS_STRING:
        *data = value.u.string->data;
        *size = value.u.string->size;
        *length = value.u.string->length;
        return bsNull();
    case BS_NUMBER:
        *size = bsNumberFormat(value.u.number, buffer, bufferSize);
        *data = buffer;
        *length = *size;
        return bsNull();
    case BS_NULL:
        *data = "null";
        *size = 4;
        *length = 4;
        return bsNull();
    case BS_BOOLEAN:
        *data = value.u.boolean ? "true" : "false";
        *size = value.u.boolean ? 4 : 5;
        *length = *size;
        return bsNull();
    default: {
        BSValue text = bsValueString(value);
        *data = text.u.string->data;
        *size = text.u.string->size;
        *length = text.u.string->length;
        return text;
    }
    }
}


BSValue bsStringConcat(BSValue left, BSValue right)
{
    char leftBuffer[64];
    char rightBuffer[64];
    const char *leftData;
    const char *rightData;
    size_t leftSize;
    size_t rightSize;
    size_t leftLength;
    size_t rightLength;
    BSValue leftText = bsStringBytes(left, leftBuffer, sizeof(leftBuffer), &leftData, &leftSize,
                                     &leftLength);
    BSValue rightText = bsStringBytes(right, rightBuffer, sizeof(rightBuffer), &rightData, &rightSize,
                                      &rightLength);

    BSString *string = bsStringAlloc(leftSize + rightSize);
    memcpy(string->data, leftData, leftSize);
    memcpy(string->data + leftSize, rightData, rightSize);
    string->length = leftLength + rightLength;

    bsRelease(leftText);
    bsRelease(rightText);
    return bsStringTake(string);
}


const char *bsStringData(BSValue value)
{
    return value.type == BS_STRING ? value.u.string->data : "";
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

static void bsStringIndexBuild(BSString *string)
{
    size_t length = string->length;
    size_t markCount = length / BS_STRING_INDEX_STRIDE + 1;
    uint32_t *offsets = bsAlloc(markCount * sizeof(uint32_t));
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
    string->offsets = offsets;
}


size_t bsStringOffsetSlow(BSValue value, size_t index)
{
    BSString *string = value.u.string;
    size_t length = string->length;
    if (index >= length) {
        string->cursorIndex = (uint32_t) length;
        string->cursorOffset = (uint32_t) string->size;
        return string->size;
    }

    size_t position;
    size_t offset;
    if (index >= string->cursorIndex) {
        position = string->cursorIndex;
        offset = string->cursorOffset;
    } else {
        position = 0;
        offset = 0;
        if (length >= BS_STRING_INDEX_STRIDE * 2) {
            if (string->offsets == NULL) {
                bsStringIndexBuild(string);
            }
            size_t mark = index / BS_STRING_INDEX_STRIDE;
            position = mark * BS_STRING_INDEX_STRIDE;
            offset = string->offsets[mark];
        }
    }
    while (offset < string->size && position < index) {
        size_t codeSize;
        bsUTF8Decode(string->data, string->size, offset, &codeSize);
        offset += codeSize;
        position++;
    }
    string->cursorIndex = (uint32_t) position;
    string->cursorOffset = (uint32_t) offset;
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
 * The string builder
 */


/* Resolve a value to its string bytes - see the definition below */
static BSValue bsStringBytes(BSValue value, char *buffer, size_t bufferSize, const char **data,
                             size_t *size, size_t *length);


void bsSBInit(BSStringBuilder *sb)
{
    sb->data = NULL;
    sb->size = 0;
    sb->capacity = 0;
}


void bsSBFree(BSStringBuilder *sb)
{
    free(sb->data);
    bsSBInit(sb);
}


static void bsSBReserve(BSStringBuilder *sb, size_t size)
{
    if (sb->size + size + 1 > sb->capacity) {
        size_t capacity = sb->capacity != 0 ? sb->capacity : 32;
        while (capacity < sb->size + size + 1) {
            capacity *= 2;
        }
        sb->data = bsRealloc(sb->data, capacity);
        sb->capacity = capacity;
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
    va_list argsCopy;
    va_copy(argsCopy, args);
    int size = vsnprintf(NULL, 0, format, argsCopy);
    va_end(argsCopy);
    if (size > 0) {
        bsSBReserve(sb, (size_t) size);
        vsnprintf(sb->data + sb->size, (size_t) size + 1, format, args);
        sb->size += (size_t) size;
    }
    va_end(args);
}


void bsSBAppendValue(BSStringBuilder *sb, BSValue value)
{
    char buffer[64];
    const char *data;
    size_t size;
    size_t length;
    BSValue text = bsStringBytes(value, buffer, sizeof(buffer), &data, &size, &length);
    bsSBAppend(sb, data, size);
    bsRelease(text);
}


BSValue bsSBToValue(BSStringBuilder *sb)
{
    BSValue value = bsStringNewSize(sb->data != NULL ? sb->data : "", sb->size);
    bsSBFree(sb);
    return value;
}


/*
 * Array values
 */


static BSArray *bsArrayPool;
static unsigned bsArrayPoolCount;
#define BS_ARRAY_POOL_MAX 1024

static BSArray *bsArrayAlloc(void)
{
    if (bsArrayPool != NULL) {
        BSArray *array = bsArrayPool;
        bsArrayPool = (BSArray *) array->values;
        bsArrayPoolCount--;
        return array;
    }
    return bsAlloc(sizeof(BSArray));
}

static void bsArrayRecycle(BSArray *array)
{
    if (bsArrayPoolCount >= BS_ARRAY_POOL_MAX) {
        free(array);
        return;
    }
    array->values = (BSValue *) bsArrayPool;
    bsArrayPool = array;
    bsArrayPoolCount++;
}


BSValue bsArrayNewCapacity(size_t capacity)
{
    BSArray *array = bsArrayAlloc();
    array->refcount = 1;
    array->count = 0;
    array->capacity = capacity;
    array->values = capacity != 0 ? bsAlloc(capacity * sizeof(BSValue)) : NULL;

    BSValue value;
    value.type = BS_ARRAY;
    value.u.array = array;
    return value;
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
        array->values = bsRealloc(array->values, newCapacity * sizeof(BSValue));
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
    bsRelease(array->values[index]);
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
    bsRelease(array->values[index]);
    memmove(array->values + index, array->values + index + 1, (array->count - index - 1) * sizeof(BSValue));
    array->count--;
}


BSValue bsArrayCopy(BSValue value)
{
    size_t count = bsArrayCount(value);
    BSValue copy = bsArrayNewCapacity(count);
    for (size_t ix = 0; ix < count; ix++) {
        bsArrayPush(copy, bsRetain(value.u.array->values[ix]));
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
 * Object values - a treap, a binary search tree ordered by key with a max-heap on node priority
 */


static BSObject *bsObjectPool;
static unsigned bsObjectPoolCount;
#define BS_OBJECT_POOL_MAX 8192

static BSObject *bsObjectAlloc(void)
{
    if (bsObjectPool != NULL) {
        BSObject *object = bsObjectPool;
        bsObjectPool = (BSObject *) object->insertHead;
        bsObjectPoolCount--;
        return object;
    }
    return bsAlloc(sizeof(BSObject));
}

static void bsObjectRecycle(BSObject *object)
{
    free(object->lookup);
    object->lookup = NULL;
    object->lookupMask = 0;
    if (bsObjectPoolCount >= BS_OBJECT_POOL_MAX) {
        free(object);
        return;
    }
    object->insertHead = (BSObjectNode *) bsObjectPool;
    bsObjectPool = object;
    bsObjectPoolCount++;
}


BSValue bsObjectNew(void)
{
    BSObject *object = bsObjectAlloc();
    object->refcount = 1;
    object->packed = 1;
    object->count = 0;
    object->generation = 0;
    object->root = NULL;
    object->insertHead = NULL;
    object->insertTail = NULL;
    object->lookup = NULL;
    object->lookupMask = 0;

    BSValue value;
    value.type = BS_OBJECT;
    value.u.object = object;
    return value;
}


size_t bsObjectCount(BSValue value)
{
    return value.type == BS_OBJECT ? value.u.object->count : 0;
}


/*
 * The node priority source
 *
 * A deterministic xorshift keeps object layout - and therefore test behavior - reproducible from
 * run to run while still keeping the tree balanced in expectation.
 */
static uint32_t bsObjectPriorityState = 0x9E3779B9u;

/* Recycled treap nodes - BareScript allocates and frees objects constantly */
static BSObjectNode *bsObjectNodePool;
static unsigned bsObjectNodePoolCount;
#define BS_OBJECT_NODE_POOL_MAX 8192

static BSObjectNode *bsObjectNodeAlloc(void)
{
    if (bsObjectNodePool != NULL) {
        BSObjectNode *node = bsObjectNodePool;
        bsObjectNodePool = node->left;
        bsObjectNodePoolCount--;
        return node;
    }
    return bsAlloc(sizeof(BSObjectNode));
}

static void bsObjectNodeRecycle(BSObjectNode *node)
{
    if (bsObjectNodePoolCount >= BS_OBJECT_NODE_POOL_MAX) {
        free(node);
        return;
    }
    node->left = bsObjectNodePool;
    bsObjectNodePool = node;
    bsObjectNodePoolCount++;
}

static uint32_t bsObjectPriority(void)
{
    uint32_t state = bsObjectPriorityState;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    bsObjectPriorityState = state;
    return state;
}


/*
 * Interned object keys
 *
 * Short keys are interned so lookup can compare BSString pointers instead of memcmp. The intern
 * table holds one reference; interned strings live until process exit.
 */
#define BS_INTERN_MAX 64
#define BS_INTERN_INITIAL 32

typedef struct {
    BSString *string;
    uint32_t hash;
} BSInternSlot;

static BSInternSlot bsInternInitial[BS_INTERN_INITIAL];
static BSInternSlot *bsInternSlots = bsInternInitial;
static size_t bsInternMask = BS_INTERN_INITIAL - 1;
static size_t bsInternCount;
static int bsInternHeap;

static uint32_t bsInternHash(const char *data, size_t size, bool *ascii)
{
    const unsigned char *bytes = (const unsigned char *) data;
    uint32_t hash = 2166136261u;
    uint32_t high = 0;
    size_t ix = 0;
    while (ix + 4 <= size) {
        uint32_t word;
        memcpy(&word, bytes + ix, 4);
        high |= word;
        hash ^= word;
        hash *= 16777619u;
        ix += 4;
    }
    while (ix < size) {
        unsigned char byte = bytes[ix++];
        high |= byte;
        hash ^= byte;
        hash *= 16777619u;
    }
    if (ascii != NULL) {
        *ascii = (high & 0x80808080u) == 0;
    }
    return hash;
}

static void bsInternGrow(void)
{
    size_t oldCapacity = bsInternMask + 1;
    BSInternSlot *old = bsInternSlots;
    size_t capacity = oldCapacity * 2;
    bsInternMask = capacity - 1;
    bsInternSlots = bsAlloc(capacity * sizeof(BSInternSlot));
    memset(bsInternSlots, 0, capacity * sizeof(BSInternSlot));
    for (size_t ix = 0; ix < oldCapacity; ix++) {
        BSString *string = old[ix].string;
        if (string == NULL) {
            continue;
        }
        uint32_t hash = old[ix].hash;
        for (size_t probe = 0;; probe++) {
            size_t slot = (hash + probe) & bsInternMask;
            if (bsInternSlots[slot].string == NULL) {
                bsInternSlots[slot].string = string;
                bsInternSlots[slot].hash = hash;
                break;
            }
        }
    }
    if (bsInternHeap) {
        free(old);
    }
    bsInternHeap = 1;
}

static BSString *bsInternLookupHash(const char *data, size_t size, uint32_t hash)
{
    for (size_t probe = 0;; probe++) {
        size_t slot = (hash + probe) & bsInternMask;
        BSString *string = bsInternSlots[slot].string;
        if (string == NULL) {
            return NULL;
        }
        if (bsInternSlots[slot].hash == hash && string->size == size &&
            (size == 0 || memcmp(string->data, data, size) == 0)) {
            return string;
        }
    }
}

static BSString *bsInternLookup(const char *data, size_t size)
{
    return bsInternLookupHash(data, size, bsInternHash(data, size, NULL));
}

BSValue bsStringIntern(const char *data, size_t size)
{
    if (size > BS_INTERN_MAX) {
        return bsStringNewSize(data, size);
    }
    bool ascii = false;
    uint32_t hash = bsInternHash(data, size, &ascii);
    BSString *found = bsInternLookupHash(data, size, hash);
    if (found != NULL) {
        found->refcount++;
        return bsStringTake(found);
    }
    if ((bsInternCount + 1) * 4 >= (bsInternMask + 1) * 3) {
        bsInternGrow();
    }
    BSValue value = ascii ? bsStringNewAscii(data, size) : bsStringNewSize(data, size);
    value.u.string->interned = 1;
    value.u.string->refcount++;
    for (size_t probe = 0;; probe++) {
        size_t slot = (hash + probe) & bsInternMask;
        if (bsInternSlots[slot].string == NULL) {
            bsInternSlots[slot].string = value.u.string;
            bsInternSlots[slot].hash = hash;
            bsInternCount++;
            return value;
        }
    }
}

static int bsKeyCompare(const BSString *key1, const char *key2, size_t size2)
{
    if (key1->data == key2) {
        return 0;
    }
    size_t size1 = key1->size;
    size_t size = size1 < size2 ? size1 : size2;
    int result = size != 0 ? memcmp(key1->data, key2, size) : 0;
    if (result != 0) {
        return result;
    }
    return size1 < size2 ? -1 : (size1 == size2 ? 0 : 1);
}


static BSObjectNode *bsObjectRotateRight(BSObjectNode *node)
{
    BSObjectNode *left = node->left;
    node->left = left->right;
    left->right = node;
    return left;
}


static BSObjectNode *bsObjectRotateLeft(BSObjectNode *node)
{
    BSObjectNode *right = node->right;
    node->right = right->left;
    right->left = node;
    return right;
}


#define BS_OBJECT_LOOKUP_EMPTY ((BSObjectNode *) 0)
#define BS_OBJECT_LOOKUP_TOMB  ((BSObjectNode *) (uintptr_t) 1)

static uint32_t bsPtrHash(const BSString *key)
{
    uintptr_t x = (uintptr_t) key;
    x ^= x >> 16;
    x *= 0x7feb352d;
    return (uint32_t) x;
}

static void bsObjectLookupPut(BSObject *object, BSObjectNode *node);
static void bsObjectLookupGrow(BSObject *object);

static void bsObjectLookupPut(BSObject *object, BSObjectNode *node)
{
    if (object->lookup == NULL || node->key == NULL || !node->key->interned) {
        return;
    }
    if ((object->count + 1) * 2 > object->lookupMask + 1) {
        bsObjectLookupGrow(object);
    }
    uint32_t hash = bsPtrHash(node->key);
    for (uint32_t probe = 0;; probe++) {
        uint32_t slot = (hash + probe) & object->lookupMask;
        BSObjectNode *entry = object->lookup[slot];
        if (entry == BS_OBJECT_LOOKUP_EMPTY || entry == BS_OBJECT_LOOKUP_TOMB || entry->key == node->key) {
            object->lookup[slot] = node;
            return;
        }
    }
}

static void bsObjectLookupGrow(BSObject *object)
{
    BSObjectNode **old = object->lookup;
    uint32_t oldMask = object->lookupMask;
    uint32_t capacity = old != NULL ? (oldMask + 1) * 2 : 16;
    while (capacity < (uint32_t) object->count * 2 + 2) {
        capacity *= 2;
    }
    object->lookup = bsAlloc(capacity * sizeof(BSObjectNode *));
    memset(object->lookup, 0, capacity * sizeof(BSObjectNode *));
    object->lookupMask = capacity - 1;
    if (old != NULL) {
        for (uint32_t ix = 0; ix <= oldMask; ix++) {
            BSObjectNode *node = old[ix];
            if (node != BS_OBJECT_LOOKUP_EMPTY && node != BS_OBJECT_LOOKUP_TOMB) {
                bsObjectLookupPut(object, node);
            }
        }
        free(old);
    } else {
        for (BSObjectNode *node = object->insertHead; node != NULL; node = node->insertNext) {
            bsObjectLookupPut(object, node);
        }
    }
}

static void bsObjectLookupDel(BSObject *object, BSString *interned)
{
    if (object->lookup == NULL || interned == NULL) {
        return;
    }
    uint32_t hash = bsPtrHash(interned);
    for (uint32_t probe = 0;; probe++) {
        uint32_t slot = (hash + probe) & object->lookupMask;
        BSObjectNode *entry = object->lookup[slot];
        if (entry == BS_OBJECT_LOOKUP_EMPTY) {
            return;
        }
        if (entry != BS_OBJECT_LOOKUP_TOMB && entry->key == interned) {
            object->lookup[slot] = BS_OBJECT_LOOKUP_TOMB;
            return;
        }
    }
}

static BSObjectNode *bsObjectLookupGet(const BSObject *object, BSString *interned)
{
    uint32_t hash = bsPtrHash(interned);
    for (uint32_t probe = 0;; probe++) {
        uint32_t slot = (hash + probe) & object->lookupMask;
        BSObjectNode *entry = object->lookup[slot];
        if (entry == BS_OBJECT_LOOKUP_EMPTY) {
            return NULL;
        }
        if (entry != BS_OBJECT_LOOKUP_TOMB && entry->key == interned) {
            return entry;
        }
    }
}


/* Tiny objects store up to four pairs in the object itself. Past that they become a list, and
 * past BS_OBJECT_SMALL they grow a treap. */
#define BS_OBJECT_PACKED 4

/* Small objects are faster to scan in insertion order than to chase treap pointers.
 * Insert stays on the insertion-order list until the object grows past this. */
#define BS_OBJECT_SMALL 32

static BSObjectNode *bsObjectNodeCreate(BSValue key, BSValue item, BSObject *object);
static BSObjectNode *bsObjectFindKey(BSObject *object, const char *key, size_t size,
                                     BSString *interned);

static int bsObjectPackedFind(const BSObject *object, const char *key, size_t size,
                              BSString *interned)
{
    for (size_t ix = 0; ix < object->count; ix++) {
        if (interned != NULL) {
            if (object->smallKeys[ix] == interned) {
                return (int) ix;
            }
        } else if (object->smallKeys[ix]->size == size &&
                   memcmp(object->smallKeys[ix]->data, key, size) == 0) {
            return (int) ix;
        }
    }
    return -1;
}


static BSValue *bsObjectFindValue(BSObject *object, const char *key, size_t size,
                                  BSString *interned)
{
    if (object->packed) {
        if (interned == NULL && size <= BS_INTERN_MAX) {
            interned = bsInternLookup(key, size);
            if (interned == NULL) {
                return NULL;
            }
        }
        int found = bsObjectPackedFind(object, key, size, interned);
        return found >= 0 ? &object->smallValues[found] : NULL;
    }
    BSObjectNode *node = bsObjectFindKey(object, key, size, interned);
    return node != NULL ? &node->value : NULL;
}

static void bsObjectSpill(BSObject *object)
{
    BSString *keys[BS_OBJECT_PACKED];
    BSValue values[BS_OBJECT_PACKED];
    size_t n = object->count;
    for (size_t ix = 0; ix < n; ix++) {
        keys[ix] = object->smallKeys[ix];
        values[ix] = object->smallValues[ix];
    }
    object->packed = 0;
    object->count = 0;
    object->root = NULL;
    object->insertHead = NULL;
    object->insertTail = NULL;
    object->lookup = NULL;
    object->lookupMask = 0;
    for (size_t ix = 0; ix < n; ix++) {
        BSValue key = bsStringTake(keys[ix]);
        bsObjectNodeCreate(key, values[ix], object);
        bsRelease(key);
    }
}

static BSObjectNode *bsObjectNodeCreate(BSValue key, BSValue item, BSObject *object)
{
    BSObjectNode *created = bsObjectNodeAlloc();
    created->left = NULL;
    created->right = NULL;
    created->priority = bsObjectPriority();
    created->key = bsRetain(key).u.string;
    created->value = item;

    created->insertPrev = object->insertTail;
    created->insertNext = NULL;
    if (object->insertTail != NULL) {
        object->insertTail->insertNext = created;
    } else {
        object->insertHead = created;
    }
    object->insertTail = created;

    object->count++;
    object->generation++;
    bsObjectLookupPut(object, created);
    return created;
}


/* Link an existing list node into the treap. Does not touch the insertion-order list. */
static void bsObjectTreapLink(BSObject *object, BSObjectNode *created)
{
    created->left = NULL;
    created->right = NULL;
    if (object->root == NULL) {
        object->root = created;
        return;
    }

    const char *keyData = created->key->data;
    size_t keySize = created->key->size;
    BSObjectNode *path[128];
    signed char dirs[128];
    int depth = 0;
    BSObjectNode *node = object->root;
    for (;;) {
        int compare = bsKeyCompare(node->key, keyData, keySize);
        /* GCOV_EXCL_START */
        if (compare == 0 || depth >= 128) {
            abort();
        }
        /* GCOV_EXCL_STOP */
        path[depth] = node;
        if (compare > 0) {
            dirs[depth] = -1;
            if (node->left == NULL) {
                node->left = created;
                depth++;
                break;
            }
            node = node->left;
        } else {
            dirs[depth] = 1;
            if (node->right == NULL) {
                node->right = created;
                depth++;
                break;
            }
            node = node->right;
        }
        depth++;
    }

    while (depth > 0) {
        int d = depth - 1;
        BSObjectNode *parent = path[d];
        BSObjectNode *child = dirs[d] < 0 ? parent->left : parent->right;
        if (child->priority <= parent->priority) {
            break;
        }
        BSObjectNode *rotated = dirs[d] < 0 ? bsObjectRotateRight(parent) : bsObjectRotateLeft(parent);
        if (d == 0) {
            object->root = rotated;
        } else if (dirs[d - 1] < 0) {
            path[d - 1]->left = rotated;
        } else {
            path[d - 1]->right = rotated;
        }
        depth--;
    }
}


static void bsObjectBuildTreap(BSObject *object)
{
    object->root = NULL;
    for (BSObjectNode *node = object->insertHead; node != NULL; node = node->insertNext) {
        bsObjectTreapLink(object, node);
    }
}


static void bsObjectTreapInsert(BSObject *object, BSValue key, BSValue item)
{
    const char *keyData = bsStringData(key);
    size_t keySize = bsStringSize(key);
    BSObjectNode *path[128];
    signed char dirs[128];
    int depth = 0;
    BSObjectNode *node = object->root;
    for (;;) {
        int compare = bsKeyCompare(node->key, keyData, keySize);
        if (compare == 0) {
            bsRelease(node->value);
            node->value = item;
            object->generation++;
            return;
        }
        /* GCOV_EXCL_START */
        if (depth >= 128) {
            abort();
        }
        /* GCOV_EXCL_STOP */
        path[depth] = node;
        if (compare > 0) {
            dirs[depth] = -1;
            if (node->left == NULL) {
                node->left = bsObjectNodeCreate(key, item, object);
                depth++;
                break;
            }
            node = node->left;
        } else {
            dirs[depth] = 1;
            if (node->right == NULL) {
                node->right = bsObjectNodeCreate(key, item, object);
                depth++;
                break;
            }
            node = node->right;
        }
        depth++;
    }

    while (depth > 0) {
        int d = depth - 1;
        BSObjectNode *parent = path[d];
        BSObjectNode *child = dirs[d] < 0 ? parent->left : parent->right;
        if (child->priority <= parent->priority) {
            break;
        }
        BSObjectNode *rotated = dirs[d] < 0 ? bsObjectRotateRight(parent) : bsObjectRotateLeft(parent);
        if (d == 0) {
            object->root = rotated;
        } else if (dirs[d - 1] < 0) {
            path[d - 1]->left = rotated;
        } else {
            path[d - 1]->right = rotated;
        }
        depth--;
    }
}


/* Insert or update a key. Takes ownership of "item"; retains "key" if a node is created.
 * Short keys must already be interned. Objects at or under BS_OBJECT_SMALL stay a list. */
static void bsObjectInsert(BSObject *object, BSValue key, BSValue item)
{
    if (object->packed) {
        BSString *interned = (key.type == BS_STRING && key.u.string->interned) ? key.u.string : NULL;
        const char *keyData = bsStringData(key);
        size_t keySize = bsStringSize(key);
        int found = bsObjectPackedFind(object, keyData, keySize, interned);
        if (found >= 0) {
            bsRelease(object->smallValues[found]);
            object->smallValues[found] = item;
            object->generation++;
            return;
        }
        if (object->count < BS_OBJECT_PACKED) {
            object->smallKeys[object->count] = bsRetain(key).u.string;
            object->smallValues[object->count] = item;
            object->count++;
            object->generation++;
            return;
        }
        bsObjectSpill(object);
    }
    if (object->root == NULL) {
        BSString *interned = (key.type == BS_STRING && key.u.string->interned) ? key.u.string : NULL;
        const char *keyData = bsStringData(key);
        size_t keySize = bsStringSize(key);
        for (BSObjectNode *node = object->insertHead; node != NULL; node = node->insertNext) {
            if (interned != NULL) {
                if (node->key == interned) {
                    bsRelease(node->value);
                    node->value = item;
                    object->generation++;
                    return;
                }
            } else if (node->key->size == keySize &&
                       (keySize == 0 || memcmp(node->key->data, keyData, keySize) == 0)) {
                bsRelease(node->value);
                node->value = item;
                object->generation++;
                return;
            }
        }
        bsObjectNodeCreate(key, item, object);
        if (object->count > BS_OBJECT_SMALL) {
            bsObjectBuildTreap(object);
        }
        return;
    }
    bsObjectTreapInsert(object, key, item);
}


static BSObjectNode *bsObjectFind(BSObjectNode *node, const char *key, size_t size)
{
    while (node != NULL) {
        int compare = bsKeyCompare(node->key, key, size);
        if (compare == 0) {
            return node;
        }
        node = compare > 0 ? node->left : node->right;
    }
    return NULL;
}


static BSObjectNode *bsObjectFindKey(BSObject *object, const char *key, size_t size,
                                     BSString *interned)
{
    if (interned == NULL && size <= BS_INTERN_MAX) {
        interned = bsInternLookup(key, size);
        if (interned == NULL) {
            return NULL;
        }
        key = interned->data;
        size = interned->size;
    } else if (interned != NULL) {
        key = interned->data;
        size = interned->size;
    }
    if (interned != NULL && object->count > BS_OBJECT_SMALL) {
        if (object->lookup == NULL) {
            bsObjectLookupGrow(object);
        }
        return bsObjectLookupGet(object, interned);
    }
    if (object->count <= BS_OBJECT_SMALL) {
        for (BSObjectNode *node = object->insertHead; node != NULL; node = node->insertNext) {
            if (interned != NULL) {
                if (node->key == interned) {
                    return node;
                }
            } else if (node->key->size == size && (size == 0 || memcmp(node->key->data, key, size) == 0)) {
                return node;
            }
        }
        return NULL;
    }
    return bsObjectFind(object->root, key, size);
}


static BSObjectNode *bsObjectRemove(BSObjectNode *node, const char *key, size_t size, bool *removed,
                                    BSObject *object)
{
    if (node == NULL) {
        return NULL;
    }
    int compare = bsKeyCompare(node->key, key, size);
    if (compare > 0) {
        node->left = bsObjectRemove(node->left, key, size, removed, object);
        return node;
    }
    if (compare < 0) {
        node->right = bsObjectRemove(node->right, key, size, removed, object);
        return node;
    }

    /* Rotate the node down until it is a leaf, then unlink it */
    if (node->left == NULL && node->right == NULL) {
        if (node->insertPrev != NULL) {
            node->insertPrev->insertNext = node->insertNext;
        } else {
            object->insertHead = node->insertNext;
        }
        if (node->insertNext != NULL) {
            node->insertNext->insertPrev = node->insertPrev;
        } else {
            object->insertTail = node->insertPrev;
        }
        bsObjectLookupDel(object, node->key);
        bsRelease(bsStringTake(node->key));
        bsRelease(node->value);
        bsObjectNodeRecycle(node);
        object->count--;
        object->generation++;
        *removed = true;
        return NULL;
    }
    if (node->right == NULL || (node->left != NULL && node->left->priority > node->right->priority)) {
        node = bsObjectRotateRight(node);
        node->right = bsObjectRemove(node->right, key, size, removed, object);
    } else {
        node = bsObjectRotateLeft(node);
        node->left = bsObjectRemove(node->left, key, size, removed, object);
    }
    return node;
}


static void bsObjectNodesFree(BSObject *object)
{
    if (object->packed) {
        for (size_t ix = 0; ix < object->count; ix++) {
            bsRelease(bsStringTake(object->smallKeys[ix]));
            bsRelease(object->smallValues[ix]);
        }
        return;
    }
    BSObjectNode *node = object->insertHead;
    while (node != NULL) {
        BSObjectNode *next = node->insertNext;
        bsRelease(bsStringTake(node->key));
        bsRelease(node->value);
        bsObjectNodeRecycle(node);
        node = next;
    }
}


void bsObjectSetString(BSValue value, BSValue key, BSValue item)
{
    BSObject *object = value.u.object;
    if (key.type == BS_STRING && !key.u.string->interned && key.u.string->size <= BS_INTERN_MAX) {
        BSValue interned = bsStringIntern(key.u.string->data, key.u.string->size);
        bsObjectInsert(object, interned, item);
        bsRelease(interned);
        return;
    }
    bsObjectInsert(object, key, item);
}


void bsObjectSet(BSValue value, const char *key, BSValue item)
{
    BSValue keyValue = bsStringIntern(key, strlen(key));
    bsObjectSetString(value, keyValue, item);
    bsRelease(keyValue);
}


bool bsObjectLookup(BSValue object, const char *key, size_t size, BSValue *out)
{
    if (object.type != BS_OBJECT) {
        return false;
    }
    BSValue *found = bsObjectFindValue(object.u.object, key, size, NULL);
    if (found == NULL) {
        return false;
    }
    *out = *found;
    return true;
}


bool bsObjectLookupString(BSValue object, BSValue key, BSValue *out)
{
    if (object.type != BS_OBJECT) {
        return false;
    }
    BSString *interned = (key.type == BS_STRING && key.u.string->interned) ? key.u.string : NULL;
    BSValue *found = bsObjectFindValue(object.u.object, bsStringData(key), bsStringSize(key), interned);
    if (found == NULL) {
        return false;
    }
    *out = *found;
    return true;
}


BSValue *bsObjectValuePtr(BSValue object, const char *key, size_t size)
{
    return bsObjectFindValue(object.u.object, key, size, NULL);
}


BSValue bsObjectGetString(BSValue value, BSValue key)
{
    BSValue found;
    return bsObjectLookupString(value, key, &found) ? found : bsNull();
}


BSValue bsObjectGet(BSValue value, const char *key)
{
    BSValue found;
    return bsObjectLookup(value, key, strlen(key), &found) ? found : bsNull();
}


bool bsObjectHasString(BSValue value, BSValue key)
{
    if (value.type != BS_OBJECT) {
        return false;
    }
    BSString *interned = (key.type == BS_STRING && key.u.string->interned) ? key.u.string : NULL;
    return bsObjectFindValue(value.u.object, bsStringData(key), bsStringSize(key), interned) != NULL;
}


bool bsObjectHas(BSValue value, const char *key)
{
    return value.type == BS_OBJECT && bsObjectFindValue(value.u.object, key, strlen(key), NULL) != NULL;
}


bool bsObjectDelete(BSValue value, const char *key)
{
    BSObject *object = value.u.object;
    size_t size = strlen(key);
    BSString *interned = NULL;
    if (size <= BS_INTERN_MAX) {
        interned = bsInternLookup(key, size);
        if (interned == NULL) {
            return false;
        }
        key = interned->data;
        size = interned->size;
    }
    if (object->packed) {
        int found = bsObjectPackedFind(object, key, size, interned);
        if (found < 0) {
            return false;
        }
        bsRelease(bsStringTake(object->smallKeys[found]));
        bsRelease(object->smallValues[found]);
        object->count--;
        for (size_t ix = (size_t) found; ix < object->count; ix++) {
            object->smallKeys[ix] = object->smallKeys[ix + 1];
            object->smallValues[ix] = object->smallValues[ix + 1];
        }
        object->generation++;
        return true;
    }
    if (object->root == NULL) {
        BSObjectNode *node = bsObjectFindKey(object, key, size, interned);
        if (node == NULL) {
            return false;
        }
        if (node->insertPrev != NULL) {
            node->insertPrev->insertNext = node->insertNext;
        } else {
            object->insertHead = node->insertNext;
        }
        if (node->insertNext != NULL) {
            node->insertNext->insertPrev = node->insertPrev;
        } else {
            object->insertTail = node->insertPrev;
        }
        bsObjectLookupDel(object, node->key);
        bsRelease(bsStringTake(node->key));
        bsRelease(node->value);
        bsObjectNodeRecycle(node);
        object->count--;
        object->generation++;
        return true;
    }
    bool removed = false;
    object->root = bsObjectRemove(object->root, key, size, &removed, object);
    return removed;
}


typedef struct BSObjectIterContext {
    BSObjectIterFn iter;
    void *data;
} BSObjectIterContext;


static bool bsObjectIterNode(BSObjectNode *node, const BSObjectIterContext *context)
{
    if (node == NULL) {
        return true;
    }
    if (!bsObjectIterNode(node->left, context)) {
        return false;
    }
    if (!context->iter(bsStringTake(node->key), node->value, context->data)) {
        return false;
    }
    return bsObjectIterNode(node->right, context);
}


static bool bsObjectIterPackedSorted(BSObject *object, BSObjectIterFn iter, void *data)
{
    size_t n = object->count;
    size_t order[BS_OBJECT_PACKED];
    for (size_t ix = 0; ix < n; ix++) {
        order[ix] = ix;
    }
    for (size_t i = 1; i < n; i++) {
        size_t item = order[i];
        size_t j = i;
        while (j > 0) {
            BSString *left = object->smallKeys[order[j - 1]];
            BSString *right = object->smallKeys[item];
            if (bsKeyCompare(left, right->data, right->size) <= 0) {
                break;
            }
            order[j] = order[j - 1];
            j--;
        }
        order[j] = item;
    }
    for (size_t ix = 0; ix < n; ix++) {
        size_t k = order[ix];
        if (!iter(bsStringTake(object->smallKeys[k]), object->smallValues[k], data)) {
            return false;
        }
    }
    return true;
}


bool bsObjectIterSorted(BSValue value, BSObjectIterFn iter, void *data)
{
    if (value.type != BS_OBJECT) {
        return true;
    }
    if (value.u.object->packed) {
        return bsObjectIterPackedSorted(value.u.object, iter, data);
    }
    if (value.u.object->root == NULL && value.u.object->insertHead != NULL) {
        bsObjectBuildTreap(value.u.object);
    }
    BSObjectIterContext context = {iter, data};
    return bsObjectIterNode(value.u.object->root, &context);
}


bool bsObjectIter(BSValue value, BSObjectIterFn iter, void *data)
{
    if (value.type != BS_OBJECT) {
        return true;
    }
    if (value.u.object->packed) {
        BSObject *object = value.u.object;
        for (size_t ix = 0; ix < object->count; ix++) {
            if (!iter(bsStringTake(object->smallKeys[ix]), object->smallValues[ix], data)) {
                return false;
            }
        }
        return true;
    }
    for (BSObjectNode *node = value.u.object->insertHead; node != NULL; node = node->insertNext) {
        if (!iter(bsStringTake(node->key), node->value, data)) {
            return false;
        }
    }
    return true;
}


static bool bsObjectKeysIter(BSValue key, BSValue item, void *data)
{
    (void) item;
    bsArrayPush(*((BSValue *) data), bsRetain(key));
    return true;
}


BSValue bsObjectKeys(BSValue value)
{
    BSValue keys = bsArrayNewCapacity(bsObjectCount(value));
    bsObjectIter(value, bsObjectKeysIter, &keys);
    return keys;
}


BSValue bsObjectKeysSorted(BSValue value)
{
    BSValue keys = bsArrayNewCapacity(bsObjectCount(value));
    bsObjectIterSorted(value, bsObjectKeysIter, &keys);
    return keys;
}


static bool bsObjectCopyIter(BSValue key, BSValue item, void *data)
{
    bsObjectSetString(*((BSValue *) data), key, bsRetain(item));
    return true;
}


BSValue bsObjectCopy(BSValue value)
{
    BSValue copy = bsObjectNew();
    bsObjectIter(value, bsObjectCopyIter, &copy);
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
    function->name = bsStringNew(name).u.string;
    function->intrinsic = 0;

    BSValue value;
    value.type = BS_FUNCTION;
    value.u.function = function;
    return value;
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
    /* Immediate values need no work. Heap objects all start with an int32_t refcount. */
    if (value.type >= BS_STRING && value.type <= BS_FUNCTION) {
        (*(int32_t *) value.u.ref)++;
    } else if (value.type == BS_REGEX) {
        bsRegexRetain(value);
    }
    return value;
}


void bsReleaseDestroyed(BSValue value)
{
    switch (value.type) {
    case BS_STRING:
        free(value.u.string->offsets);
        free(value.u.string);
        break;
    case BS_ARRAY: {
        BSArray *array = value.u.array;
        for (size_t ix = 0; ix < array->count; ix++) {
            bsRelease(array->values[ix]);
        }
        free(array->values);
        bsArrayRecycle(array);
        break;
    }
    case BS_OBJECT: {
        BSObject *object = value.u.object;
        bsObjectNodesFree(object);
        bsObjectRecycle(object);
        break;
    }
    default: {
        BSFunction *function = value.u.function;
        if (function->dataFree != NULL) {
            function->dataFree(function->data);
        }
        bsRelease(bsStringTake(function->name));
        free(function);
        break;
    }
    }
}


void bsRelease(BSValue value)
{
    if (value.type == BS_REGEX) {
        bsRegexRelease(value);
        return;
    }
    if (value.type < BS_STRING || value.type > BS_FUNCTION) {
        return;
    }
    if (--(*(int32_t *) value.u.ref) != 0) {
        return;
    }
    bsReleaseDestroyed(value);
}


void bsAssign(BSValue *target, BSValue value)
{
    BSValue previous = *target;
    *target = value;
    bsRelease(previous);
}


/*
 * Number formatting - JavaScript's Number.prototype.toString
 */


size_t bsNumberFormat(double number, char *buffer, size_t bufferSize)
{
    if (isnan(number)) {
        return (size_t) snprintf(buffer, bufferSize, "NaN");
    }
    if (isinf(number)) {
        return (size_t) snprintf(buffer, bufferSize, number > 0 ? "Infinity" : "-Infinity");
    }
    if (number == 0) {
        return (size_t) snprintf(buffer, bufferSize, "0");
    }

    /*
     * Integers in the range that prints without an exponent - the common case by far - format with
     * a digit loop, skipping the round-trip search below entirely
     */
    if (number == trunc(number) && number > -1e15 && number < 1e15) {
        char integerText[24];
        size_t integerSize = 0;
        bool negative = number < 0;
        uint64_t magnitude = (uint64_t) (negative ? -number : number);
        do {
            integerText[integerSize++] = (char) ('0' + (magnitude % 10));
            magnitude /= 10;
        } while (magnitude != 0);
        char text[26];
        size_t size = 0;
        if (negative) {
            text[size++] = '-';
        }
        while (integerSize != 0) {
            text[size++] = integerText[--integerSize];
        }
        text[size] = '\0';
        return (size_t) snprintf(buffer, bufferSize, "%s", text);
    }

    /*
     * Find the shortest round-tripping decimal representation
     *
     * Round-tripping is monotone in precision - if p digits round-trip then so do p + 1 - so the
     * shortest precision is found by binary search rather than by trying each in turn.
     */
    char digits[40];
    int exponent = 0;
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
    exponent = (int) strtol(cursor + 1, NULL, 10);

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
    mantissa[digitCount] = '\0';

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
        text[size++] = 'e';
        text[size++] = (n - 1 >= 0 ? '+' : '-');
        size += (size_t) snprintf(text + size, sizeof(text) - size, "%d", n - 1 >= 0 ? n - 1 : -(n - 1));
    }
    text[size] = '\0';
    return (size_t) snprintf(buffer, bufferSize, "%s", text);
}


double bsNumberRound(double number, int digits)
{
    double multiplier = pow(10, digits);
    double scaled = number * multiplier;
    if (!isfinite(scaled)) {
        return number; /* GCOV_EXCL_LINE */
    }
    double rounded = trunc(scaled + (number >= 0 ? 0.5 : -0.5));
    return rounded / multiplier;
}


bool bsNumberParse(const char *text, size_t size, double *result)
{
    /* ^\s*[-+]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][-+]?[0-9]+)?\s*$ */
    size_t ix = 0;
    while (ix < size && isspace((unsigned char) text[ix])) {
        ix++;
    }
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
        size_t save = ix;
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
            ix = save;
            return false;
        }
    }
    size_t end = ix;
    while (ix < size && isspace((unsigned char) text[ix])) {
        ix++;
    }
    if (ix != size) {
        return false;
    }

    char buffer[64];
    size_t numberSize = end - begin;
    if (numberSize >= sizeof(buffer)) {
        return false;
    }
    memcpy(buffer, text + begin, numberSize);
    buffer[numberSize] = '\0';
    double value = strtod(buffer, NULL);
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
    size_t ix = 0;
    while (ix < size && isspace((unsigned char) text[ix])) {
        ix++;
    }
    bool negative = false;
    if (ix < size && (text[ix] == '-' || text[ix] == '+')) {
        negative = (text[ix] == '-');
        ix++;
    }
    size_t digits = 0;
    double value = 0;
    while (ix < size) {
        char ch = text[ix];
        int digit;
        if (ch >= '0' && ch <= '9') {
            digit = ch - '0';
        } else if (ch >= 'a' && ch <= 'z') {
            digit = ch - 'a' + 10;
        } else if (ch >= 'A' && ch <= 'Z') {
            digit = ch - 'A' + 10;
        } else {
            break;
        }
        if (digit >= radix) {
            break;
        }
        value = value * radix + digit;
        digits++;
        ix++;
    }
    if (digits == 0) {
        return false;
    }
    while (ix < size && isspace((unsigned char) text[ix])) {
        ix++;
    }
    if (ix != size || !isfinite(value)) {
        return false;
    }
    *result = negative ? -value : value;
    return true;
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


int64_t bsDatetimeFromParts(double year, double month, double day, double hour, double minute,
                            double second, double millisecond)
{
    int64_t yearInt = (int64_t) year;
    int64_t monthInt = (int64_t) month;
    int64_t dayInt = (int64_t) day;
    int64_t hourInt = (int64_t) hour;
    int64_t minuteInt = (int64_t) minute;
    int64_t secondInt = (int64_t) second;
    int64_t millisecondInt = (int64_t) millisecond;

    /* Cascade the out-of-range time components, matching the reference implementation */
    if (millisecondInt < 0 || millisecondInt >= 1000) {
        int64_t extra = bsFloorDiv(millisecondInt, 1000);
        millisecondInt -= extra * 1000;
        secondInt += extra;
    }
    if (secondInt < 0 || secondInt >= 60) {
        int64_t extra = bsFloorDiv(secondInt, 60);
        secondInt -= extra * 60;
        minuteInt += extra;
    }
    if (minuteInt < 0 || minuteInt >= 60) {
        int64_t extra = bsFloorDiv(minuteInt, 60);
        minuteInt -= extra * 60;
        hourInt += extra;
    }
    if (hourInt < 0 || hourInt >= 24) {
        int64_t extra = bsFloorDiv(hourInt, 24);
        hourInt -= extra * 24;
        dayInt += extra;
    }
    if (monthInt < 1 || monthInt > 12) {
        int64_t extra = bsFloorDiv(monthInt - 1, 12);
        monthInt -= extra * 12;
        yearInt += extra;
    }

    int64_t civil = bsDaysFromCivil(yearInt, monthInt, dayInt) * 86400 +
        hourInt * 3600 + minuteInt * 60 + secondInt;
    int64_t utc = civil - bsLocalOffset(civil);
    utc = civil - bsLocalOffset(utc);
    return utc * 1000 + millisecondInt;
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
    return bsDatetimeFromParts(parts.year, parts.month, parts.day, 0, 0, 0, 0);
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
    int year, month, day;

    /* Date form - "YYYY-MM-DD" - is a local-time date */
    if (size == 10) {
        if (text[4] != '-' || text[7] != '-' ||
            !bsParseDigits(text, 0, 4, &year) || !bsParseDigits(text, 5, 2, &month) ||
            !bsParseDigits(text, 8, 2, &day)) {
            return false;
        }
        if (month < 1 || month > 12 || day < 1 || day > 31) {
            return false;
        }
        *result = bsDatetimeFromParts(year, month, day, 0, 0, 0, 0);
        return true;
    }

    /* Datetime form - "YYYY-MM-DDTHH:MM:SS[.fff](Z|(+|-)HH:MM)" */
    if (size < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':') {
        return false;
    }
    int hour, minute, second;
    if (!bsParseDigits(text, 0, 4, &year) || !bsParseDigits(text, 5, 2, &month) ||
        !bsParseDigits(text, 8, 2, &day) || !bsParseDigits(text, 11, 2, &hour) ||
        !bsParseDigits(text, 14, 2, &minute) || !bsParseDigits(text, 17, 2, &second)) {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 24 || minute > 59 || second > 59) {
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


const char *bsValueTypeString(BSValue value)
{
    switch (value.type) {
    case BS_NULL:
        return "null";
    case BS_BOOLEAN:
        return "boolean";
    case BS_NUMBER:
        return "number";
    case BS_DATETIME:
        return "datetime";
    case BS_STRING:
        return "string";
    case BS_ARRAY:
        return "array";
    case BS_OBJECT:
        return "object";
    case BS_FUNCTION:
        return "function";
    default:
        return "regex";
    }
}


BSValue bsValueString(BSValue value)
{
    char buffer[64];
    switch (value.type) {
    case BS_NULL:
        return bsStringNew("null");
    case BS_BOOLEAN:
        return bsStringNew(value.u.boolean ? "true" : "false");
    case BS_NUMBER:
        bsNumberFormat(value.u.number, buffer, sizeof(buffer));
        return bsStringNew(buffer);
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
        return bsRetain(value);
    case BS_ARRAY:
    case BS_OBJECT:
        return bsJSONEncode(value, 0);
    case BS_FUNCTION:
        return bsStringNew("<function>");
    default:
        return bsStringNew("<regex>");
    }
}


BSValue bsValueJSON(BSValue value, int indent)
{
    return bsJSONEncode(value, indent);
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
    if (value1.type == BS_NUMBER && value2.type == BS_NUMBER) {
        return value1.u.number == value2.u.number;
    }
    if (value1.type != value2.type) {
        return false;
    }
    switch (value1.type) {
    case BS_NULL:
        return true;
    case BS_BOOLEAN:
        return value1.u.boolean == value2.u.boolean;
    case BS_DATETIME:
        return value1.u.datetime == value2.u.datetime;
    default:
        return value1.u.ref == value2.u.ref;
    }
}


static int bsCompareNumbers(double left, double right)
{
    return left < right ? -1 : (left == right ? 0 : 1);
}


int bsValueCompare(BSValue left, BSValue right)
{
    if (left.type == BS_NULL) {
        return right.type == BS_NULL ? 0 : -1;
    }
    if (right.type == BS_NULL) {
        return 1;
    }
    if (left.type == BS_STRING && right.type == BS_STRING) {
        int compare = bsKeyCompare(left.u.string, right.u.string->data, right.u.string->size);
        return compare < 0 ? -1 : (compare == 0 ? 0 : 1);
    }
    if (left.type == BS_BOOLEAN && right.type == BS_BOOLEAN) {
        return bsCompareNumbers(left.u.boolean ? 1 : 0, right.u.boolean ? 1 : 0);
    }
    if (left.type == BS_NUMBER && right.type == BS_NUMBER) {
        return bsCompareNumbers(left.u.number, right.u.number);
    }
    if (left.type == BS_DATETIME && right.type == BS_DATETIME) {
        return left.u.datetime < right.u.datetime ? -1 : (left.u.datetime == right.u.datetime ? 0 : 1);
    }
    if (left.type == BS_ARRAY && right.type == BS_ARRAY) {
        size_t leftCount = left.u.array->count;
        size_t rightCount = right.u.array->count;
        size_t count = leftCount < rightCount ? leftCount : rightCount;
        for (size_t ix = 0; ix < count; ix++) {
            int compare = bsValueCompare(left.u.array->values[ix], right.u.array->values[ix]);
            if (compare != 0) {
                return compare;
            }
        }
        return leftCount < rightCount ? -1 : (leftCount == rightCount ? 0 : 1);
    }
    if (left.type == BS_OBJECT && right.type == BS_OBJECT) {
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
        bsRelease(leftKeys);
        bsRelease(rightKeys);
        if (result != 0) {
            return result;
        }
        return leftCount < rightCount ? -1 : (leftCount == rightCount ? 0 : 1);
    }

    /* Values of different types compare by type name */
    int typeCompare = strcmp(bsValueTypeString(left), bsValueTypeString(right));
    return typeCompare < 0 ? -1 : (typeCompare == 0 ? 0 : 1);
}
