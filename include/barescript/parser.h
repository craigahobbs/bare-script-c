/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The BareScript parser and the compiled script representation
 *
 * Parsing is done by barescriptParser.bare, an include library script that runs on this runtime and
 * produces the JSON "BareScript model" - so the parser, and the exact syntax and error messages it
 * accepts, are shared with the JavaScript and Python implementations. The model is compiled to
 * bytecode; the original model is kept for lint and coverage.
 *
 * Structured statements - if/elif/else, while, for, break, continue - never reach the runtime; the
 * parser lowers them to labels and jumps, which compile to JUMP / JUMPIF.
 */

#ifndef BARESCRIPT_PARSER_H
#define BARESCRIPT_PARSER_H

#include "value.h"
#include "export.h"

#ifdef __cplusplus
extern "C" {
#endif

BS_VISIBILITY_BEGIN


typedef struct BSExpr BSExpr;
typedef struct BSScript BSScript;
typedef struct BSFunctionDef BSFunctionDef;


/* An include script reference compiled into a code chunk */
typedef struct BSInclude {
    BSValue url;
    bool system;
} BSInclude;


/*
 * Call-site cache of a global function lookup
 *
 * The cache points at the globals object's value slot for the name, so an assignment to the name
 * is seen through the slot. The slot is re-resolved when the globals object's structural
 * generation changes (a key added or removed) or the options instance changes.
 */
typedef struct BSCallCache {
    BSValue *slot;      /* the value slot in the globals object, or NULL if the name is absent */
    uint32_t gen;       /* the globals object's structural generation the slot was resolved at */
    uint32_t epoch;     /* the options instance the slot was resolved for */
    uint32_t nameIndex; /* the function name's constant index */
} BSCallCache;


/*
 * A compiled bytecode chunk
 *
 * Instructions are (opcode << 24) | arg. Constants are interned names, string literals, and
 * numbers. STMT operands index cover[], borrowed statement models from the parser output.
 * CALL_NAME operands index caches[], one per call site. stackMax is the deepest value stack the
 * chunk can reach, computed at emit time, so the interpreter allocates the stack once.
 */
typedef struct BSCode {
    uint32_t *inst;
    size_t count;
    size_t stackMax;
    BSValue *constants;
    size_t constantCount;
    BSCallCache *caches;
    size_t cacheCount;
    BSInclude *includes;
    size_t includeCount;
    BSValue *cover;
    int *coverLines;
    uint32_t *coverPcs;  /* per statement, the index of its first instruction */
    size_t coverCount;
    BSValue *slotNames;
    size_t slotCount;
} BSCode;


/* A compiled expression - a bytecode chunk plus the original expression model */
struct BSExpr {
    BSCode code;
    BSValue model;
};


/* A function definition */
struct BSFunctionDef {
    BSValue name;
    BSValue *argNames;
    size_t argCount;
    bool lastArgArray;
    bool async;
    BSCode code;
    BSScript *script;
    int lineNumber;
    int lineCount;
};


/* A parsed script */
struct BSScript {
    int32_t refcount;
    BSValue scriptName;
    BSValue scriptLines;
    BSValue model;          /* the original parser model, for lint and coverage */
    BSCode code;            /* top-level bytecode */
    BSFunctionDef **functions;
    size_t functionCount;
    bool system;

    /*
     * Coverage recording cache. Hits are counted in a line-indexed array of pointers into the
     * coverage object's per-line count values, so a loop does not format line keys or search the
     * covered object on every statement. coverageOwner is the coverage global's identity;
     * coverageCovered is borrowed from it.
     */
    BSObject *coverageOwner;
    BSValue coverageCovered;
    BSValue **coverageCounts;
    int coverageLineCap;
};


/* A parser error */
typedef struct BSParserError {
    BSValue error;        /* the error description string */
    BSValue line;         /* the error line text */
    BSValue scriptName;   /* the script name string, or a null value */
    int columnNumber;
    int lineNumber;       /* zero if unknown */
    BSValue message;      /* the formatted, multi-line error message */
} BSParserError;

void bsParserErrorFree(BSParserError *error);


/*
 * Parse a BareScript script. Returns the parsed script, or NULL on error, in which case "error"
 * is filled in and must be freed with bsParserErrorFree.
 */
BSScript *bsParseScript(const char *text, size_t size, int startLineNumber, const char *scriptName,
                        BSParserError *error);

/*
 * Lint a parsed script with barescriptLint.bare. "globals" is the globals object the script would
 * execute against, used to resolve function references, or a null value. Returns an owned array of
 * warning strings.
 */
BSValue bsLintScript(const BSScript *script, BSValue globals);

/* Release the parser and linter include library scripts - call at process exit */
void bsParserCleanup(void);

/*
 * Parse a BareScript expression. Returns the parsed expression, or NULL on error, in which case
 * "error" is filled in and must be freed with bsParserErrorFree. If "arrayLiterals" is true,
 * "[...]" parses as an array literal rather than a bracketed variable name.
 */
BSExpr *bsParseExpression(const char *text, size_t size, int lineNumber, const char *scriptName,
                          bool arrayLiterals, BSParserError *error);


/*
 * Model conversion
 */

/* Convert a JSON "BareScript" model to a compiled script; NULL if the model is invalid */
BSScript *bsScriptFromModel(BSValue model, const char *scriptName);

/* Convert a JSON "Expression" model to a compiled expression; NULL if the model is invalid */
BSExpr *bsExprFromModel(BSValue model);

/* Convert a compiled script to its JSON "BareScript" model - returns an owned object value */
BSValue bsScriptToModel(const BSScript *script);

/* Convert a compiled expression to its JSON "Expression" model - returns an owned object value */
BSValue bsExprToModel(const BSExpr *expr);


/* Script reference counting */
BSScript *bsScriptRetain(BSScript *script);
void bsScriptRelease(BSScript *script);

/* Free a standalone expression parsed with bsParseExpression */
void bsExprFree(BSExpr *expr);


BS_VISIBILITY_END

#ifdef __cplusplus
}
#endif

#endif
