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


/* Allocation helpers - these abort the process on allocation failure */
void *bsAlloc(size_t size);
void *bsRealloc(void *ptr, size_t size);
char *bsStrdup(const char *text);

/* Inflate a bundled include model. Returns a NUL-terminated, malloc-allocated buffer, or NULL. */
char *bsGzipUncompress(const unsigned char *src, size_t srcSize);

/* Compiled bundled include, cached after the first load. Returns an owned script reference. */
BSScript *bsIncludeScript(const char *name);


/* Regex value reference counting - implemented by the regex engine */
void bsRegexRetain(BSValue value);
void bsRegexRelease(BSValue value);

/* Destroy a heap value whose refcount has reached zero */
void bsReleaseDestroyed(BSValue value);

/*
 * Fast-path retain/release for implementation files. Immediate values are a no-op the compiler
 * can see; the public functions in value.c remain the library ABI.
 */
static inline BSValue bsRetainInline(BSValue value)
{
    if (value.type >= BS_STRING && value.type <= BS_FUNCTION) {
        (*(int32_t *) value.u.ref)++;
    } else if (value.type == BS_REGEX) {
        bsRegexRetain(value);
    }
    return value;
}

static inline void bsReleaseInline(BSValue value)
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

#ifndef BARESCRIPT_VALUE_IMPL
#define bsRetain bsRetainInline
#define bsRelease bsReleaseInline
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


/* Run a compiled bytecode chunk. Returns an owned value. */
BSValue bsRunCode(const BSCode *code, BSScript *script, BSOptions *options, BSScope *scope,
                  bool builtins);

/* Drop the saved parser model from a cached system include (not linted or covered) */
void bsScriptDropModel(BSScript *script);

/* Free a function definition */
void bsFunctionDefFree(BSFunctionDef *def);

void bsCodeFree(BSCode *code);


/* Bytecode: instruction is (opcode << 24) | 24-bit argument */
#define BS_OP(inst) ((uint8_t) ((inst) >> 24))
#define BS_ARG(inst) ((uint32_t) ((inst) & 0xffffffu))
#define BS_INST(op, arg) (((uint32_t) (op) << 24) | ((uint32_t) (arg) & 0xffffffu))

enum {
    BS_OP_LOAD_NULL = 0,
    BS_OP_LOAD_TRUE,
    BS_OP_LOAD_FALSE,
    BS_OP_LOAD_CONST,
    BS_OP_LOAD_SLOT,
    BS_OP_LOAD_NAME,
    BS_OP_STORE_SLOT,
    BS_OP_STORE_NAME,
    BS_OP_POP,
    BS_OP_DUP,
    BS_OP_JUMP,
    BS_OP_JUMP_FALSE,
    BS_OP_JUMP_TRUE,
    BS_OP_JUMP_UNDEF,
    BS_OP_RETURN,
    BS_OP_CALL_NAME,
    BS_OP_CALL_SLOT,
    BS_OP_ADD,
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
    BS_OP_NEG,
    BS_OP_NOT,
    BS_OP_BNOT,
    BS_OP_FUNCTION,
    BS_OP_INCLUDE,
    BS_OP_STMT,
    BS_OP_ARGC = 0xFF  /* follows CALL_*; argument count, never dispatched */
};


/* The script function closure data - a function value created by a function definition statement */
typedef struct BSScriptFunction {
    BSScript *script;
    BSFunctionDef *def;
} BSScriptFunction;


/*
 * Look up an object key. Returns true if the key is present; "*out" is then a borrowed value
 * (which may itself be null). Distinguishes a missing key from a key whose value is null.
 */
bool bsObjectLookup(BSValue object, const char *key, size_t size, BSValue *out);

/* Lookup by a string value. An interned key skips intern-table hashing. */
bool bsObjectLookupString(BSValue object, BSValue key, BSValue *out);

/* Pointer to the stored value for key, or NULL if absent. Valid until a key is added or removed. */
BSValue *bsObjectValuePtr(BSValue object, const char *key, size_t size);
BSValue *bsObjectValuePtrString(BSValue object, BSValue key);

/*
 * Append a key known to be absent, skipping the duplicate scan. Takes ownership of "item" and
 * retains "key". For building an object from keys that are distinct by construction.
 */
void bsObjectAppend(BSValue object, BSValue key, BSValue item);

/* Intern a short string. Strings longer than 64 bytes are not interned. Returns an owned value. */
BSValue bsStringIntern(const char *data, size_t size);

/* Reuse an interned string if present; otherwise a new ordinary string. Does not grow the table. */
BSValue bsStringInternExisting(const char *data, size_t size);

/* Allocate a string whose bytes are already known to be ASCII (length == size). */
BSValue bsStringNewAscii(const char *text, size_t size);

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
    BS_INTRIN_OBJECT_COPY,
    BS_INTRIN_OBJECT_DELETE,
    BS_INTRIN_OBJECT_GET,
    BS_INTRIN_OBJECT_HAS,
    BS_INTRIN_OBJECT_KEYS,
    BS_INTRIN_OBJECT_SET,
    BS_INTRIN_OBJECT_NEW,
    BS_INTRIN_STRING_ENDS_WITH,
    BS_INTRIN_STRING_LENGTH,
    BS_INTRIN_STRING_STARTS_WITH,
    BS_INTRIN_SYSTEM_BOOLEAN,
    BS_INTRIN_SYSTEM_TYPE,
    BS_INTRIN_REGEX_MATCH
};

/* Happy-path regexMatch; the argument types must already be regex and string */
BSValue bsRegexMatchImpl(BSValue regex, BSValue string);

/* A value's interned type name string - "array", "boolean", ... - as an owned value */
BSValue bsSystemTypeName(BSValue value);

/* The value type names, indexed by BSType */
extern const char *const bsTypeNames[BS_REGEX + 1];


/* A capture group's interned name string value (borrowed), or a null value if the group is unnamed */
BSValue bsRegexGroupNameValue(BSValue regex, size_t group);


#endif
