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


/* Release the calling thread's HTTP connection pool (options.c) */
void bsFetchCleanup(void);


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
        if ((count) == (cap)) { \
            (cap) = (cap) != 0 ? (cap) * 2 : (initial); \
            (array) = bsRealloc((array), (cap) * sizeof(*(array))); \
        } \
    } while (0)

/* Inflate a bundled include model. Returns a NUL-terminated, malloc-allocated buffer, or NULL. */
char *bsGzipUncompress(const unsigned char *src, size_t srcSize);

/* Compiled bundled include, cached after the first load. Returns an owned script reference. */
BSScript *bsIncludeScript(const char *name);


/* Regex value reference counting - implemented by the regex engine */
void bsRegexDestroy(BSValue value);

/* Free the thread's regex match scratch buffers (regex.c) */
void bsRegexScratchFree(void);

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


/* Drop the saved parser model from a cached system include (not linted or covered) */
void bsScriptDropModel(BSScript *script);

/* Re-parse a script's retained lines into a fresh model. Returns an owned model, or a null value. */
BSValue bsScriptReparse(const BSScript *script);

/* Bring back a forgotten model and the chunks' borrowed statement models, for coverage recording */
bool bsScriptRestoreCover(BSScript *script);

/*
 * The syntax tree the emitter compiles
 *
 * A statement is loaded into an arena of nodes - from the parser's model objects, or straight
 * from model JSON, which then builds no objects at all - emitted, and the arena reset for the
 * next, so the arena holds one statement at a time. A node's links are indexes into the arena,
 * zero meaning none; its string is an index plus one into the arena's interned strings.
 */
enum {
    BS_NODE_NUMBER = 1,   /* number */
    BS_NODE_STRING,       /* text: the literal */
    BS_NODE_VARIABLE,     /* text: the name */
    BS_NODE_CALL,         /* text: the function name; a: the first argument; b: the argument count */
    BS_NODE_BINARY,       /* op: the opcode, or BS_NODE_AND / BS_NODE_OR; a, b: the operands */
    BS_NODE_UNARY,        /* op: the opcode; a: the operand */
    BS_NODE_GROUP,        /* a: the expression */
    BS_NODE_EXPR,         /* a: the expression; text: the name assigned, or none */
    BS_NODE_JUMP,         /* text: the label; a: the condition, or none */
    BS_NODE_RETURN,       /* a: the expression, or none */
    BS_NODE_LABEL,        /* text: the name */
    BS_NODE_FUNCTION,     /* text: the name; a: the first statement; b: the first argument; flag: lastArgArray */
    BS_NODE_INCLUDE,      /* a: the first include */
    BS_NODE_INCLUDE_ITEM, /* text: the url; flag: system */
    BS_NODE_ARG           /* text: a function argument's name */
};

/* The short-circuit operators, in a binary node's op past the opcodes */
#define BS_NODE_AND 0xFE
#define BS_NODE_OR 0xFF

typedef struct BSNode {
    uint8_t kind;
    uint8_t flag;
    uint16_t op;
    uint32_t next;  /* the next node of a list, or zero */
    uint32_t a;
    uint32_t b;
    uint32_t text;
    int32_t line;   /* a statement's line number, or zero */
    double number;
    BSValue model;  /* a statement's model object, borrowed, for coverage - or a null value */
} BSNode;

typedef struct BSAst {
    BSNode *nodes;   /* node zero is unused, so a zero link means none */
    uint32_t count;
    uint32_t capacity;
    BSValue *strings;
    uint32_t stringCount;
    uint32_t stringCapacity;
} BSAst;

void bsAstInit(BSAst *ast);
void bsAstReset(BSAst *ast);  /* release the strings and empty the arena, keeping its buffers */
void bsAstFree(BSAst *ast);

/* A new node, zeroed, by index - the arena may move, so re-index after one */
uint32_t bsAstNode(BSAst *ast, uint8_t kind);

/* Take an owned string into the arena. Returns its index plus one, a node's "text". */
uint32_t bsAstString(BSAst *ast, BSValue string);

/* A binary node's op for an operator string - an opcode or a short-circuit code - and a unary node's opcode; zero if unknown */
uint16_t bsBinaryNodeOp(const char *op);
uint8_t bsUnaryOpcode(const char *op);

/* Load a statement or expression model object into the arena. Returns the node, or zero for a malformed model. */
uint32_t bsAstStatement(BSAst *ast, BSValue model);
uint32_t bsAstExpr(BSAst *ast, BSValue model);

/*
 * Decode a script model's JSON - {"statements": [...], ...} - a statement at a time: each element
 * of "statements" is read into "ast", passed to "emit", and the arena reset before the next. The
 * model's other members are returned in "rest", owned. Returns false - with "error" set, "rest"
 * null - on a JSON error, a model without a statements array, a malformed statement, or when
 * "emit" returns false.
 */
bool bsJSONDecodeScript(const char *text, size_t size, BSAst *ast,
                        bool (*emit)(BSAst *ast, uint32_t statement, void *data), void *data,
                        BSValue *rest, const char **error);


/*
 * Bytecode: fixed eight-byte register instructions. "a" is the destination register or an index;
 * "b" and "c" are operands, each a register or - with the high bit set - a constant. A jump's
 * target is the 32-bit word "w" that overlays b and c.
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

#define BS_OPERAND_CONST 0x8000u           /* an operand naming a constant rather than a register */
#define BS_OPERAND_INDEX(o) ((o) & 0x7fffu)
#define BS_REG_DISCARD 0xffffu              /* a call destination that drops the result */
#define BS_OPERANDS_PER_DATA 3              /* call argument operands per DATA word */

enum {
    BS_OP_MOVE = 0,    /* a = b */
    BS_OP_LOAD_NAME,   /* a = the name at cache site b: the locals object's, else the global */
    BS_OP_STORE_NAME,  /* the global at cache site a = b */
    BS_OP_JUMP,        /* pc = w */
    BS_OP_JUMP_FALSE,  /* if !a: pc = w */
    BS_OP_JUMP_TRUE,   /* if a: pc = w */
    BS_OP_JUMP_UNDEF,  /* the unknown-label error for the name in constant a */
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
    BS_OP_CALL_OBJECT_SET,
    BS_OP_CALL_STRING_LENGTH,
    BS_OP_CALL_STRING_SLICE,
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


/* The script function closure data - a function value created by a function definition statement */
typedef struct BSScriptFunction {
    BSScript *script;
    BSFunctionDef *def;
} BSScriptFunction;


/*
 * Look up an object key by a string value. Returns true if the key is present; "*out" is then a
 * borrowed value (which may itself be null). Distinguishes a missing key from a key whose value is
 * null. An interned key skips intern-table hashing.
 */
bool bsObjectLookupString(BSValue object, BSValue key, BSValue *out);

/*
 * The one member of an object with exactly one key. A model node - an expression or a statement -
 * is such an object, whose member's name is its kind. Returns false for any other value; the key
 * and value are borrowed.
 */
bool bsObjectSole(BSValue object, BSString **key, BSValue *value);

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
    BS_INTRIN_STRING_LENGTH,
    BS_INTRIN_STRING_STARTS_WITH,
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
bool bsRegexGroupsNamed(BSValue regex);

/* A capture group's interned name string value (borrowed), or a null value if the group is unnamed */
BSValue bsRegexGroupNameValue(BSValue regex, size_t group);


#endif
