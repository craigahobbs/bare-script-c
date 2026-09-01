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


/*
 * The internal "unset" value
 *
 * A function-local slot holding the unset marker has not been assigned, so a variable lookup falls
 * through to the globals object - matching the reference implementations, where an unassigned
 * local simply is not a key of the locals dictionary.
 */
#define BS_UNSET_TYPE ((BSType) -1)
#define BS_IS_UNSET(value) ((int) (value).type == (int) BS_UNSET_TYPE)

BSValue bsUnset(void);


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


#endif
