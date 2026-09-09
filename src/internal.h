/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * Internal declarations shared across the BareScript implementation files
 */

#ifndef BARESCRIPT_INTERNAL_H
#define BARESCRIPT_INTERNAL_H

#include <math.h>
#include <string.h>

#include "barescript/parser.h"
#include "barescript/runtime.h"
#include "barescript/value.h"


/* The largest datetime JavaScript's Date represents, in milliseconds */
#define BS_DATETIME_MAX 8640000000000000.0

/* Keep a function out of line - a slow path whose inlining would bloat every hot call site */
#if defined(__GNUC__) || defined(__clang__)
#define BS_NOINLINE __attribute__((noinline))
#else
#define BS_NOINLINE
#endif

/* Allocation helpers - these abort the process on allocation failure */
void *bsAlloc(size_t size);
void *bsRealloc(void *ptr, size_t size);
char *bsStrdup(const char *text);

/* Make room in "array" - "count" elements of "cap" allocated - for one more, doubling when it is full */
#define BS_GROW(array, count, cap, initial) \
    do { \
        if ((count) >= (cap)) { \
            (cap) = (cap) != 0 ? (cap) * 2 : (initial); \
            (array) = bsRealloc((array), (cap) * sizeof(*(array))); \
        } \
    } while (0)

/* Inflate a bundled include model. Returns a NUL-terminated, malloc-allocated buffer, or NULL. */
unsigned char *bsGzipUncompress(const unsigned char *src, size_t srcSize, size_t *size);

/* Compiled bundled include, cached after the first load. Returns an owned script reference. */
BSScript *bsIncludeScript(const char *name);


/* Regex value reference counting - implemented by the regex engine */
void bsRegexDestroy(BSValue value);

/* Free the thread's regex match scratch buffers (regex.c) */
void bsRegexScratchFree(void);

/* Release the calling thread's HTTP connection pool (options.c) */
void bsFetchCleanup(void);

/* Destroy a heap value whose refcount has reached zero */
void bsReleaseDestroyed(BSValue value);

/*
 * Fast-path retain/release for implementation files. Immediate values are a no-op the compiler
 * can see; the public functions in value.c remain the library ABI.
 *
 * Every reference type - string, array, object, function, regex - begins with the same int32_t
 * refcount, so one unsigned range test over the contiguous BS_STRING..BS_REGEX span decides
 * whether a value is counted at all, and one decrement serves them all. The regex used to be
 * counted through a call of its own, which cost the common non-reference value an extra compare
 * on the hottest path in the runtime.
 */
#define BS_IS_REF(value) \
    ((unsigned) (value).type - (unsigned) BS_STRING <= (unsigned) (BS_REGEX - BS_STRING))

static inline BSValue bsRetainInline(BSValue value)
{
    if (BS_IS_REF(value)) {
        (*(int32_t *) value.u.ref)++;
    }
    return value;
}

static inline void bsReleaseInline(BSValue value)
{
    if (!BS_IS_REF(value)) {
        return;
    }
    if (--(*(int32_t *) value.u.ref) != 0) {
        return;
    }
    bsReleaseDestroyed(value);
}

static inline void bsAssignInline(BSValue *target, BSValue value)
{
    BSValue previous = *target;
    *target = value;
    bsReleaseInline(previous);
}

#ifndef BARESCRIPT_VALUE_IMPL
#define bsRetain bsRetainInline
#define bsRelease bsReleaseInline
#define bsAssign bsAssignInline
#endif


/*
 * The internal "unset" value
 *
 * A function-local slot holding the unset marker has not been assigned, so a variable lookup falls
 * through to the globals object - matching the reference implementations, where an unassigned
 * local simply is not a key of the locals dictionary.
 */
#define BS_UNSET_TYPE ((BSType) -1)
#define BS_IS_UNSET(value) ((int) (value).type == (int) BS_UNSET_TYPE)

static inline BSValue bsUnset(void)
{
    BSValue value;
    value.type = BS_UNSET_TYPE;
    value.u.ref = NULL;
    return value;
}


/* Re-parse a script's retained lines into a fresh model. Returns an owned model, or a null value. */
BSValue bsScriptReparse(const BSScript *script);

/* Bring back a forgotten model and the chunks' borrowed statement models, for coverage recording */
bool bsScriptRestoreCover(BSScript *script);

/*
 * Compile a script from its binary model - the bundled include library's encoding, which
 * bin/includeSource.bare describes and writes - a statement at a time. Returns NULL for a malformed
 * model.
 */
BSScript *bsScriptFromModelBinary(const unsigned char *data, size_t size, const char *scriptName);


/*
 * Bytecode: fixed eight-byte register instructions. "a" is the destination register or an index;
 * "b" and "c" are operands, each a register - a slot, a temporary, or one of the chunk's
 * constants, which a frame copies in after the temporaries (while a chunk is emitted a constant
 * operand carries the high bit and its constant index instead, until the temporaries are
 * counted). A jump's target is the 32-bit word "w" that overlays b and c.
 */
struct BSInst {
    uint8_t op;
    uint16_t a;
    union {
        struct {
            uint16_t b;
            uint16_t c;
        };
        uint32_t w;
    };
};

#define BS_OPERAND_CONST 0x8000u           /* while emitting, an operand naming a constant rather than a register */
#define BS_OPERAND_MAX 0x7fffu             /* the largest register or constant index an operand can name */
#define BS_OPERAND_INDEX(o) ((o) & BS_OPERAND_MAX)
#define BS_REG_DISCARD 0xffffu              /* a call destination that drops the result */
#define BS_OPERANDS_PER_DATA 3              /* call argument operands per DATA word */

enum {
    BS_OP_MOVE = 0,    /* a = b */
    BS_OP_LOAD_NAME,   /* a = the name at cache site b: the locals object's, else the global */
    BS_OP_STORE_NAME,  /* the global at cache site a = b */
    BS_OP_JUMP,        /* pc = w */
    BS_OP_JUMP_FALSE,  /* if !a: pc = w */
    BS_OP_JUMP_TRUE,   /* if a: pc = w */
    BS_OP_JUMP_UNDEF,  /* the unknown-label error for the name at index a of the chunk's names */
    BS_OP_RETURN,      /* return a */
    BS_OP_CALL_NAME,   /* a = the global at site b called with the c arguments in the DATA words that follow */
    BS_OP_CALL_SLOT,   /* the same, calling the function in register b */
    BS_OP_ADD,         /* a = b op c ... */
    BS_OP_SUB,
    BS_OP_MUL,
    BS_OP_DIV,
    BS_OP_MOD,
    BS_OP_POW,
    BS_OP_EQ,
    BS_OP_NE,
    BS_OP_LT,
    BS_OP_LE,
    BS_OP_GT,
    BS_OP_GE,
    BS_OP_BAND,
    BS_OP_BOR,
    BS_OP_BXOR,
    BS_OP_SHL,
    BS_OP_SHR,
    BS_OP_NEG,         /* a = op b ... */
    BS_OP_NOT,
    BS_OP_BNOT,
    BS_OP_FUNCTION,    /* define script function a as a global */
    BS_OP_INCLUDE,     /* run the b includes from include a */
    BS_OP_STMT,        /* statement a begins */
    BS_OP_LOAD_SLOT,   /* a = slot b, or the global of its name if the slot is unset */
    /*
     * A CALL_NAME whose name is a library intrinsic's and whose argument count is its happy path's:
     * the interpreter runs the intrinsic in place once the site's global proves to be that library
     * function, and takes the general call otherwise
     */
    BS_OP_CALL_ARRAY_GET,
    BS_OP_CALL_ARRAY_LENGTH,
    BS_OP_CALL_ARRAY_PUSH,
    BS_OP_CALL_ARRAY_SET,
    BS_OP_CALL_OBJECT_GET,
    BS_OP_CALL_OBJECT_HAS,
    BS_OP_CALL_OBJECT_SET,
    BS_OP_CALL_STRING_CHAR_CODE_AT,
    BS_OP_CALL_STRING_LENGTH,
    BS_OP_CALL_STRING_SLICE,
    BS_OP_CALL_MATH,   /* one of the one-argument math intrinsics, told apart by the site's function */
    /*
     * A jump on a comparison: if b op c, pc = the target in the DATA word that follows, else past
     * that word. A jump on a comparison being true takes the comparison's own opcode, a jump on it
     * being false the opposite comparison's.
     */
    BS_OP_JUMP_EQ,
    BS_OP_JUMP_NE,
    BS_OP_JUMP_LT,
    BS_OP_JUMP_LE,
    BS_OP_JUMP_GT,
    BS_OP_JUMP_GE,
    BS_OP_DATA = 0xFF  /* call operands, a comparison jump's target, or a trap's line; never dispatched */
};



/*
 * Look up an object key by a string value. Returns true if the key is present; "*out" is then a
 * borrowed value (which may itself be null). Distinguishes a missing key from a key whose value is
 * null.
 */
bool bsObjectLookupString(BSValue object, BSValue key, BSValue *out);

/* Whether a stored key equals the string "key" - a pointer compare when both are interned */
bool bsObjectKeyIs(const BSString *stored, BSValue key);

/* Pointer to the stored value for key, or NULL if absent. Valid until a key is added or removed. */
BSValue *bsObjectValuePtr(BSValue object, const char *key, size_t size);
BSValue *bsObjectValuePtrString(BSValue object, BSValue key);

/* The entry holding a string key, or NULL if absent. Valid until a key is added or removed. */
BSObjectEntry *bsObjectEntryFind(BSObject *object, BSString *key);

/*
 * The entry holding a key, checked first at "*memo" - the entry index a call site found its key
 * at last time, which a find updates - so a record built the same way as the last one costs one
 * pointer compare. NULL if the key is absent.
 */
static inline BSObjectEntry *bsObjectEntryMemo(BSObject *object, BSString *key, uint32_t *memo)
{
    uint32_t ix = *memo;
    if (ix < object->count && object->entries[ix].key == key) {
        return &object->entries[ix];
    }
    BSObjectEntry *entry = bsObjectEntryFind(object, key);
    if (entry != NULL) {
        *memo = (uint32_t) (entry - object->entries);
    }
    return entry;
}

/*
 * Append a key known to be absent, skipping the duplicate scan. Takes ownership of "item" and
 * retains "key". For building an object from keys that are distinct by construction.
 */
void bsObjectAppend(BSValue object, BSValue key, BSValue item);

/* Intern a short string. Strings longer than 64 bytes are not interned. Returns an owned value. */
BSValue bsStringIntern(const char *data, size_t size);

/* Reuse an interned string if present; otherwise a new ordinary string. Does not grow the table. */
BSValue bsStringInternExisting(const char *data, size_t size);

/* The library's stringTrim and stringIndexOf of validated arguments, shared with the intrinsic switch (library.c) */
BSValue bsStringTrimValue(BSValue string);
BSValue bsStringIndexOfValue(BSValue string, BSValue search, size_t index);

/* An array of "size" retained copies of "value" (library.c) */
BSValue bsArrayNewSizeValue(size_t size, BSValue value);

/* Whether a name is the word - a first-character test before the compare, the names rarely being it */
static inline bool bsNameIs(const char *name, const char *word)
{
    return name[0] == word[0] && strcmp(name, word) == 0;
}

/* The same of a string value, by size and bytes, so a slice is read as it is */
static inline bool bsStringIs(BSValue value, const char *word)
{
    size_t size = strlen(word);
    return value.u.string->size == size && memcmp(value.u.string->data, word, size) == 0;
}

/*
 * Append a value's string representation to a string that nothing else references - the caller
 * holds its only reference and has checked that - growing the allocation in place. Returns the
 * string, which may have moved.
 */
BSString *bsStringAppendValue(BSString *string, BSValue value);

/* strtod over an unterminated span (value.c) */
double bsStrtod(const char *text, size_t size);

/* Allocate a string whose bytes are already known to be ASCII (length == size). */
BSValue bsStringNewAscii(const char *text, size_t size);

/*
 * A string of "size" bytes of a string at a byte offset: a short one copies, a longer one shares
 * the parent's bytes - a slice, which holds the root parent - so the parser's rest-of-the-line
 * slices and split pieces copy nothing. "length" is the span's code point count, or SIZE_MAX to
 * count it. A span shorter than BS_STRING_SLICE_MIN is copied: below it the copy costs less than
 * the parent bookkeeping, and a copy of a line-sized span reuses the blocks the last line freed.
 */
#define BS_STRING_SLICE_MIN 96
BSValue bsStringSliceShare(BSValue parent, size_t offset, size_t size, size_t length);

static inline BSValue bsStringSliceBytes(BSValue parent, size_t offset, size_t size, size_t length)
{
    const BSString *source = parent.u.string;
    if (size >= BS_STRING_SLICE_MIN) {
        return bsStringSliceShare(parent, offset, size, length);
    }
    const char *text = source->data + offset;
    return length == size || source->length == source->size ? bsStringNewAscii(text, size) :
        bsStringNewSize(text, size);
}

/*
 * A string's bytes for a size-aware reader: a slice's span is not NUL-terminated, and this does
 * not take the copy bsStringData does to terminate it
 */
static inline const char *bsStringSpan(BSValue value)
{
    return value.type == BS_STRING ? value.u.string->data : "";
}

/* Ensure a string builder has room for "size" more bytes, so a reader can fill sb->data + sb->size */
void bsSBReserve(BSStringBuilder *sb, size_t size);

/* Non-ASCII code-point index to byte offset; ASCII is handled by bsStringOffsetFast */
size_t bsStringOffsetSlow(BSValue value, size_t index);

static inline size_t bsStringOffsetFast(BSValue value, size_t index)
{
    if (value.type != BS_STRING) {
        return 0;
    }
    BSString *string = value.u.string;
    if (string->length == string->size) {
        return index < string->size ? index : string->size;
    }
    return bsStringOffsetSlow(value, index);
}

#ifndef BARESCRIPT_VALUE_IMPL
#define bsStringOffset bsStringOffsetFast
#endif


/* A 0-9 / a-z / A-Z digit's value, or -1 */
static inline int bsDigitValue(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A' + 10;
    }
    return -1;
}

static inline int bsHexValue(char ch)
{
    int value = bsDigitValue(ch);
    return value > 15 ? -1 : value;
}


/*
 * The Unicode spaces - JavaScript's WhiteSpace and LineTerminator sets: the ASCII spaces, U+00A0,
 * U+1680, U+2000-U+200A, U+2028, U+2029, U+202F, U+205F, U+3000, and U+FEFF. The regex \s class,
 * stringTrim, and number parsing all read this one set.
 */
static inline bool bsIsSpaceCode(uint32_t code)
{
    if (code < 0x80) {
        return code == ' ' || (code >= 0x09 && code <= 0x0D);
    }
    return code == 0xA0 || code == 0x1680 || (code >= 0x2000 && code <= 0x200A) || code == 0x2028 ||
        code == 0x2029 || code == 0x202F || code == 0x205F || code == 0x3000 || code == 0xFEFF;
}


/*
 * Unicode case mapping (unicode.c)
 */

/*
 * A string's full upper or lower case, as an owned string value - the sharp s upper-cases to "SS",
 * a capital sigma that ends a word lower-cases to the final sigma
 */
BSValue bsStringToCase(BSValue string, bool upper);

/*
 * JavaScript's canonical form of a code point for case-insensitive matching: its simple upper
 * case, unless that would map a non-ASCII code point to ASCII
 */
uint32_t bsUnicodeCanon(uint32_t code);

/*
 * The code points a canonical form matches case-insensitively - itself, its lower case when that
 * maps back, and the further members of its group when it has several lower-case forms - into
 * "members", which holds BS_CANON_MEMBERS; returns the count. The largest group has three members
 * past its canonical form.
 */
#define BS_CANON_MEMBERS 4
size_t bsUnicodeCanonMembers(uint32_t canon, uint32_t *members);


/* True if "string" begins or ends with "search". Both must be strings. */
static inline bool bsStringStartsWith(BSValue string, BSValue search)
{
    size_t n = search.u.string->size;
    return n <= string.u.string->size &&
           memcmp(string.u.string->data, search.u.string->data, n) == 0;
}

static inline bool bsStringEndsWith(BSValue string, BSValue search)
{
    size_t n = search.u.string->size;
    size_t size = string.u.string->size;
    return n <= size && memcmp(string.u.string->data + (size - n), search.u.string->data, n) == 0;
}


/* An owned array of retained copies of "args" */
static inline BSValue bsArrayFromArgs(const BSValue *args, size_t argCount)
{
    BSValue array = bsArrayNewCapacity(argCount);
    for (size_t ix = 0; ix < argCount; ix++) {
        bsArrayPush(array, bsRetain(args[ix]));
    }
    return array;
}


/* Intern the parser-model object keys so JSON decode and emit share interned names */
void bsModelKeysInit(void);

/* Copy src's pairs onto dest, overwriting matching keys */
void bsObjectAssign(BSValue dest, BSValue src);


/*
 * Library function fast-path identifiers, stored on BSFunction.intrinsic
 *
 * Zero means "call fn". The interpreter's call path takes the non-zero ones itself, without
 * argument-model validation on the happy path.
 */
enum {
    BS_INTRIN_NONE = 0,
    BS_INTRIN_ARRAY_COPY,
    BS_INTRIN_ARRAY_GET,
    BS_INTRIN_ARRAY_LENGTH,
    BS_INTRIN_ARRAY_POP,
    BS_INTRIN_ARRAY_PUSH,
    BS_INTRIN_ARRAY_SET,
    BS_INTRIN_ARRAY_NEW,
    BS_INTRIN_ARRAY_NEW_SIZE,
    BS_INTRIN_MATH_ABS,
    BS_INTRIN_MATH_CEIL,
    BS_INTRIN_MATH_FLOOR,
    BS_INTRIN_MATH_SIGN,
    BS_INTRIN_MATH_SQRT,
    BS_INTRIN_NUMBER_PARSE_INT,
    BS_INTRIN_OBJECT_COPY,
    BS_INTRIN_OBJECT_DELETE,
    BS_INTRIN_OBJECT_GET,
    BS_INTRIN_OBJECT_HAS,
    BS_INTRIN_OBJECT_KEYS,
    BS_INTRIN_OBJECT_SET,
    BS_INTRIN_OBJECT_NEW,
    BS_INTRIN_STRING_CHAR_CODE_AT,
    BS_INTRIN_STRING_ENDS_WITH,
    BS_INTRIN_STRING_INDEX_OF,
    BS_INTRIN_STRING_LENGTH,
    BS_INTRIN_STRING_STARTS_WITH,
    BS_INTRIN_STRING_TRIM,
    BS_INTRIN_STRING_SLICE,
    BS_INTRIN_SYSTEM_BOOLEAN,
    BS_INTRIN_SYSTEM_GLOBAL_SET,
    BS_INTRIN_SYSTEM_TYPE,
    BS_INTRIN_REGEX_MATCH
};

/* systemGlobalSet: store a value under a name in the globals and return it, owned */
BSValue bsGlobalSetValue(BSOptions *options, BSValue name, BSValue value);

/* Happy-path regexMatch; the argument types must already be regex and string */
BSValue bsRegexMatchImpl(BSValue regex, BSValue string);

/* A value's interned type name string - "array", "boolean", ... - as an owned value */
BSValue bsSystemTypeName(BSValue value);

/* The substring of a string between two code point indexes, both within it, as an owned string value */
BSValue bsStringSlice(BSValue string, size_t begin, size_t end);

/*
 * The line of the statement containing the instruction at "pc" - the last of "count" statements
 * whose first instruction, in "pcs", is at or before it - or zero before the first
 */
int bsCoverLine(const uint32_t *pcs, const int *lines, size_t count, size_t pc);

/* The value type names, indexed by BSType */
extern const char *const bsTypeNames[BS_REGEX + 1];


/* True if no two of a regex's capture groups share a name */
bool bsRegexGroupNamesUnique(BSValue regex);

/* True if any of a regex's capture groups is named */
bool bsRegexGroupsNamed(BSValue regex);

/* A capture group's interned name string value (borrowed), or a null value if the group is unnamed */
BSValue bsRegexGroupNameValue(BSValue regex, size_t group);


#endif
