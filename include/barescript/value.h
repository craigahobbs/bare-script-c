/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The BareScript value system
 *
 * Values are 16-byte tagged structs passed by value. Null, boolean, number, and datetime values
 * are immediate - they never allocate. String, array, object, function, and regex values point at
 * a reference-counted heap object.
 *
 * Reference counting rules:
 *
 * - A function that returns a BSValue returns an *owned* reference; the caller must release it.
 * - A function that takes a BSValue takes a *borrowed* reference; it must retain the value if it
 *   keeps it beyond the call.
 * - Container accessors (bsArrayGet, bsObjectGet) return *borrowed* references, valid until the
 *   container is modified or released.
 */

#ifndef BARESCRIPT_VALUE_H
#define BARESCRIPT_VALUE_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "export.h"

#ifdef __cplusplus
extern "C" {
#endif

BS_VISIBILITY_BEGIN


/* The BareScript value types */
typedef enum {
    BS_NULL = 0,
    BS_BOOLEAN,
    BS_NUMBER,
    BS_DATETIME,
    BS_STRING,
    BS_ARRAY,
    BS_OBJECT,
    BS_FUNCTION,
    BS_REGEX
} BSType;


typedef struct BSString BSString;
typedef struct BSArray BSArray;
typedef struct BSObject BSObject;
typedef struct BSFunction BSFunction;
typedef struct BSRegex BSRegex;
typedef struct BSOptions BSOptions;


/* A BareScript value */
typedef struct BSValue {
    BSType type;
    union {
        bool boolean;
        double number;
        int64_t datetime; /* milliseconds since the Unix epoch, UTC */
        BSString *string;
        BSArray *array;
        BSObject *object;
        BSFunction *function;
        BSRegex *regex;
        void *ref; /* any reference-counted payload */
    } u;
} BSValue;


/* String flags - the high bits record the allocation's recycling size class */
#define BS_STR_INTERNED 0x01u /* the intern table holds a reference */
#define BS_STR_ASCII    0x02u /* every byte is ASCII; length == size */

/* An immutable, reference-counted UTF-8 string */
struct BSString {
    int32_t refcount;
    uint8_t flags;
    uint32_t size;         /* bytes */
    uint32_t length;       /* Unicode code points */
    uint32_t cursorIndex;  /* last code-point index passed to bsStringOffsetSlow */
    uint32_t cursorOffset; /* corresponding byte offset */
    uint32_t *offsets;     /* sparse code-point-to-byte map, or NULL */
    char data[];           /* NUL-terminated UTF-8; allocated with the header */
};


/* A reference-counted array of values */
struct BSArray {
    int32_t refcount;
    size_t count;
    size_t capacity;
    BSValue *values;
};


/*
 * A reference-counted object's binary search tree node
 *
 * The tree is a treap - a binary search tree ordered by key, with each node also satisfying the
 * max-heap property on a pseudo-random priority. This keeps the tree balanced in expectation
 * without the bookkeeping of an AVL or red-black tree, which matters because BareScript code
 * routinely inserts keys in sorted order (a worst case for a plain binary search tree).
 */
typedef struct BSObjectNode {
    struct BSObjectNode *left;
    struct BSObjectNode *right;
    struct BSObjectNode *insertPrev;
    struct BSObjectNode *insertNext;
    uint32_t priority;
    BSString *key;
    BSValue value;
} BSObjectNode;


/*
 * A reference-counted object of key/value pairs
 *
 * Up to four pairs live in the object itself. Past that, keys live on an insertion-order list of
 * nodes; past 32 keys an interned-pointer hash table indexes the list, and a treap over the same
 * nodes - built only when something needs key order, or a key that must be matched by content -
 * gives the sorted traversal. The two storage forms are exclusive, so they share the object's
 * storage. Iteration is insertion order - matching the reference implementations, whose objects
 * are JavaScript objects and Python dictionaries. JSON encoding and value comparison walk sorted
 * keys.
 */
struct BSObject {
    int32_t refcount;
    uint8_t packed;      /* 1 = u.small, 0 = u.tree - an insertion list, indexed past 32 keys */
    uint8_t uninterned;  /* 1 if any key is not interned; interned hash miss then walks the treap */
    uint32_t count;
    uint32_t generation; /* incremented when a key is added or removed - value slots then move */
    union {
        struct {
            BSString *keys[4];
            BSValue values[4];
        } small;
        struct {
            BSObjectNode *root;
            BSObjectNode *insertHead;
            BSObjectNode *insertTail;
            /*
             * Open-addressing table of interned keys to nodes, built once the object outgrows the
             * insertion-order scan. NULL until then. Tombstones are a sentinel pointer.
             */
            BSObjectNode **lookup;
            uint32_t lookupMask;
        } tree;
    } u;
};


/*
 * The BareScript function implementation signature
 *
 * "args" is a borrowed array of "argCount" argument values. The function returns an owned value
 * reference. To signal a runtime error - which halts the script - call bsErrorSet and return
 * bsNull().
 */
typedef BSValue (*BSFunctionFn)(const BSValue *args, size_t argCount, BSOptions *options, void *data);


/* A reference-counted function value */
struct BSFunction {
    int32_t refcount;
    BSFunctionFn fn;
    void *data;                  /* the function's closure data */
    void (*dataFree)(void *data);/* the closure data destructor, or NULL */
    BSString *name;              /* the function name, for error messages */
    unsigned char intrinsic;     /* 0 = none; otherwise a library fast-path id */
};


/*
 * Value constructors
 */

BSValue bsNull(void);
BSValue bsBoolean(bool value);
BSValue bsNumber(double value);
BSValue bsDatetime(int64_t milliseconds);
BSValue bsStringNew(const char *text);
BSValue bsStringNewSize(const char *text, size_t size);
BSValue bsStringNewFormat(const char *format, ...);
BSValue bsStringNewVFormat(const char *format, va_list args);
BSValue bsStringTake(BSString *string); /* takes ownership of the string reference */
BSValue bsArrayNew(void);
BSValue bsArrayNewCapacity(size_t capacity);
BSValue bsObjectNew(void);

/* A new object expecting "count" keys - past the small-object threshold it is born with its lookup table */
BSValue bsObjectNewCapacity(size_t count);
BSValue bsFunctionNew(const char *name, BSFunctionFn fn, void *data, void (*dataFree)(void *data));


/*
 * Reference counting
 */

BSValue bsRetain(BSValue value);
void bsRelease(BSValue value);

/* Release "*target" and replace it with the owned reference "value" */
void bsAssign(BSValue *target, BSValue value);


/*
 * Value accessors
 */

/* Get a value's type string ('array', 'boolean', ...) */
const char *bsValueTypeString(BSValue value);

/* Get a value's string representation - returns an owned string value */
BSValue bsValueString(BSValue value);

/* Interpret a value as a boolean */
bool bsValueBoolean(BSValue value);

/* Test if one value is the same object as another */
bool bsValueIs(BSValue value1, BSValue value2);

/* Compare two values - -1 if less than, 0 if equal, 1 if greater than */
int bsValueCompare(BSValue left, BSValue right);


/*
 * String values
 */

const char *bsStringData(BSValue value);
size_t bsStringSize(BSValue value);   /* the size, in bytes */
size_t bsStringLength(BSValue value); /* the length, in Unicode code points */

/*
 * Concatenate two values' string representations - returns an owned string value
 *
 * The result is allocated once, at its final size, so the "+" operator does not pay for a growable
 * buffer and an intermediate string on every concatenation.
 */
BSValue bsStringConcat(BSValue left, BSValue right);

/* Get the byte offset of a code point index; returns the string size if out of range */
size_t bsStringOffset(BSValue value, size_t index);

/* Get the code point at a code point index */
uint32_t bsStringCodePoint(BSValue value, size_t index);

/* Append the UTF-8 encoding of a code point to a buffer; returns the number of bytes written */
size_t bsUTF8Encode(uint32_t codePoint, char *buffer);

/* Decode the code point at "offset"; sets "*size" to the encoding's byte size */
uint32_t bsUTF8Decode(const char *data, size_t size, size_t offset, size_t *codeSize);

/* Count a UTF-8 buffer's code points; returns SIZE_MAX if the buffer is not valid UTF-8 */
size_t bsUTF8Length(const char *data, size_t size);


/*
 * A growable string builder
 */

typedef struct BSStringBuilder {
    char *data;
    size_t size;
    size_t capacity;
} BSStringBuilder;

void bsSBInit(BSStringBuilder *sb);
void bsSBFree(BSStringBuilder *sb);
void bsSBAppend(BSStringBuilder *sb, const char *text, size_t size);
void bsSBAppendString(BSStringBuilder *sb, const char *text);
void bsSBAppendChar(BSStringBuilder *sb, char ch);
void bsSBAppendFormat(BSStringBuilder *sb, const char *format, ...);
void bsSBAppendValue(BSStringBuilder *sb, BSValue value);

/* Convert the builder to a string value, freeing the builder */
BSValue bsSBToValue(BSStringBuilder *sb);


/*
 * Array values
 */

size_t bsArrayCount(BSValue value);
BSValue bsArrayGet(BSValue value, size_t index);          /* borrowed */
void bsArraySet(BSValue value, size_t index, BSValue item); /* takes ownership of "item" */
void bsArrayPush(BSValue value, BSValue item);            /* takes ownership of "item" */
void bsArrayInsert(BSValue value, size_t index, BSValue item);
void bsArrayDelete(BSValue value, size_t index);
void bsArrayReserve(BSValue value, size_t capacity);
BSValue bsArrayCopy(BSValue value);
void bsArraySort(BSValue value, int (*compare)(BSValue, BSValue, void *), void *data);


/*
 * Object values
 */

size_t bsObjectCount(BSValue value);
BSValue bsObjectGet(BSValue value, const char *key);           /* borrowed; bsNull() if absent */
BSValue bsObjectGetString(BSValue value, BSValue key);         /* borrowed; bsNull() if absent */
bool bsObjectHas(BSValue value, const char *key);
bool bsObjectHasString(BSValue value, BSValue key);
void bsObjectSet(BSValue value, const char *key, BSValue item);       /* takes ownership of "item" */
void bsObjectSetString(BSValue value, BSValue key, BSValue item);     /* takes ownership of "item" */
bool bsObjectDelete(BSValue value, const char *key);
BSValue bsObjectKeys(BSValue value);       /* an owned array of the keys, in insertion order */
BSValue bsObjectKeysSorted(BSValue value); /* an owned array of the keys, in sorted key order */
BSValue bsObjectCopy(BSValue value);

/* Iterate an object's key/value pairs in insertion order. Return false to stop iteration. */
typedef bool (*BSObjectIterFn)(BSValue key, BSValue item, void *data);
bool bsObjectIter(BSValue value, BSObjectIterFn iter, void *data);

/* Iterate an object's key/value pairs in sorted key order. Return false to stop iteration. */
bool bsObjectIterSorted(BSValue value, BSObjectIterFn iter, void *data);


/*
 * Number values
 */

/* Round a number to "digits" decimal digits */
double bsNumberRound(double value, int digits);

/* Parse a number string; returns false if parsing fails */
bool bsNumberParse(const char *text, size_t size, double *result);

/* Parse an integer string of the given radix (2 - 36); returns false if parsing fails */
bool bsIntegerParse(const char *text, size_t size, int radix, double *result);

/* Format a number the way JavaScript's Number.prototype.toString does */
size_t bsNumberFormat(double value, char *buffer, size_t bufferSize);


/*
 * Datetime values
 */

/* The local-time broken-down form of a datetime */
typedef struct BSDatetimeParts {
    int year;
    int month;       /* 1 - 12 */
    int day;         /* 1 - 31 */
    int hour;        /* 0 - 23 */
    int minute;      /* 0 - 59 */
    int second;      /* 0 - 59 */
    int millisecond; /* 0 - 999 */
    int tzOffset;    /* minutes west of UTC, matching JavaScript's getTimezoneOffset */
} BSDatetimeParts;

/* Convert a datetime to its local-time parts */
void bsDatetimeParts(int64_t milliseconds, BSDatetimeParts *parts);

/* Create a datetime from local-time components, normalizing out-of-range values */
int64_t bsDatetimeFromParts(double year, double month, double day, double hour, double minute,
                            double second, double millisecond);

/* Parse an ISO date/time string; returns false if parsing fails */
bool bsDatetimeParse(const char *text, size_t size, int64_t *result);

/* Get the current datetime */
int64_t bsDatetimeNow(void);

/* Get today's datetime (local midnight) */
int64_t bsDatetimeToday(void);


/*
 * Function values
 */

/* Call a function value. Returns an owned reference; check bsErrorGet(options) for runtime errors. */
BSValue bsFunctionCall(BSValue function, const BSValue *args, size_t argCount, BSOptions *options);


BS_VISIBILITY_END

#ifdef __cplusplus
}
#endif

#endif
