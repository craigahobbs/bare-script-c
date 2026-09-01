/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * Internal declarations shared across the BareScript implementation files
 */

#ifndef BARESCRIPT_INTERNAL_H
#define BARESCRIPT_INTERNAL_H

#include "barescript/parser.h"
#include "barescript/runtime.h"
#include "barescript/value.h"


/* Allocation helpers - these abort the process on allocation failure */
void *bsAlloc(size_t size);
void *bsRealloc(void *ptr, size_t size);
char *bsStrdup(const char *text);


/* The binary and unary operator text, indexed by operator - used by model conversion */
extern const char *bsBinaryOpText[BS_BINARY_COUNT];
extern const char *bsUnaryOpText[BS_UNARY_COUNT];


/* Regex value reference counting - implemented by the regex engine */
void bsRegexRetain(BSValue value);
void bsRegexRelease(BSValue value);

/* Destroy a heap value whose refcount has reached zero */
void bsReleaseDestroyed(BSValue value);

#ifndef BARESCRIPT_VALUE_IMPL
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


/* Statement and expression execution, shared by the runtime and the parser's model conversion */
BSValue bsExecuteStatements(BSScript *script, BSStatement **statements, size_t statementCount,
                            BSOptions *options, BSScope *scope);

/* Free a parsed statement list */
void bsStatementsFree(BSStatement **statements, size_t statementCount);

/* Free a function definition */
void bsFunctionDefFree(BSFunctionDef *def);


/* The script function closure data - a function value created by a function definition statement */
typedef struct BSScriptFunction {
    BSScript *script;
    BSFunctionDef *def;
} BSScriptFunction;

/* The script function implementation */
BSValue bsScriptFunctionCall(const BSValue *args, size_t argCount, BSOptions *options, void *data);


/*
 * Look up an object key. Returns true if the key is present; "*out" is then a borrowed value
 * (which may itself be null). Distinguishes a missing key from a key whose value is null.
 */
bool bsObjectLookup(BSValue object, const char *key, size_t size, BSValue *out);


/*
 * Library function fast-path identifiers, stored on BSFunction.intrinsic
 *
 * Zero means "call fn". Non-zero values are handled by bsFunctionInvoke without going through
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
    BS_INTRIN_STRING_ENDS_WITH,
    BS_INTRIN_STRING_LENGTH,
    BS_INTRIN_STRING_STARTS_WITH,
    BS_INTRIN_SYSTEM_BOOLEAN,
    BS_INTRIN_SYSTEM_TYPE
};

/* Call a function, taking a library intrinsic fast path when the function has one */
BSValue bsFunctionInvoke(BSValue function, const BSValue *args, size_t argCount, BSOptions *options);


#endif
