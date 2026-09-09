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
 * parser lowers them to labels and jumps, which compile to JUMP, JUMP_TRUE, JUMP_FALSE, and the
 * comparison jumps JUMP_EQ through JUMP_GE.
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
 * Per-site cache of a global name lookup - a CALL_NAME function, or a LOAD_NAME or STORE_NAME variable
 *
 * The cache points at the globals object's value slot for the name, so an assignment to the name
 * is seen through the slot. The slot is re-resolved when the globals object's structural
 * generation changes (a key added or removed) or the options instance changes. An objectGet or
 * objectSet call site also remembers the entry index its key was found at, since records built
 * the same way keep a key at the same index; the entry's key is checked before the memo is used.
 */
typedef struct BSCallCache {
    BSValue *slot;      /* the value slot in the globals object, or NULL if the name is absent */
    uint32_t gen;       /* the globals object's structural generation the slot was resolved at */
    uint32_t epoch;     /* the options instance the slot was resolved for */
    uint32_t nameIndex; /* the name's index in the chunk's names */
    uint32_t memo;      /* the entry index an object call site last found its key at */
} BSCallCache;


/*
 * A compiled bytecode chunk
 *
 * Instructions are eight-byte register instructions whose operands name a register - a slot, a
 * temporary, or a constant (a string literal or a number) copied in after them; the names call,
 * load, and store sites refer to are in names[]. STMT operands index cover[], borrowed statement
 * models from the parser output - NULL once the script forgets its model, until coverage recording
 * restores them. CALL_NAME, LOAD_NAME, and STORE_NAME operands index caches[], one per site.
 * tempCount is the temporaries the chunk needs past its slots, computed at emit time, so the
 * interpreter allocates its registers once.
 */
typedef struct BSInst BSInst;

typedef struct BSCode {
    BSInst *inst;
    size_t count;
    size_t tempCount;    /* the temporary registers past the slots */
    BSValue *constants;  /* the operand constants - literals - which a frame copies in after its temporaries */
    size_t constantCount;
    BSValue *names;      /* the names the name sites and the unknown-label traps refer to */
    size_t nameCount;
    BSCallCache *caches;
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
    size_t argCount;      /* the arguments are the first slots of the code */
    bool lastArgArray;
    BSCode code;
    BSValue *frame;       /* the resident frame - registers kept between calls, from the second call */
    bool frameBusy;       /* the resident frame is in use, so a recursive call builds its own */
    bool called;          /* the function has been called once */
};


/* A parsed script */
struct BSScript {
    int32_t refcount;
    BSValue scriptName;
    BSValue scriptLines;
    int startLineNumber;    /* the first script line's line number - bsScriptToModel re-parses with it */
    BSValue model;          /* the parser model, if kept - see bsScriptForgetModel */
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

/* The same, parsing a string value */
BSScript *bsParseScriptString(BSValue text, int startLineNumber, const char *scriptName,
                              BSParserError *error);

/*
 * Lint a parsed script with barescriptLint.bare. "globals" is the globals object the script would
 * execute against, used to resolve function references, or a null value. Returns an owned array of
 * warning strings.
 */
BSValue bsLintScript(const BSScript *script, BSValue globals);

/* Release the calling thread's parser and linter include library scripts - call at thread exit */
void bsParserCleanup(void);

/*
 * Parse a BareScript expression. Returns the parsed expression, or NULL on error, in which case
 * "error" is filled in and must be freed with bsParserErrorFree. A "lineNumber" of 0 means the
 * expression has no line. If "arrayLiterals" is true, "[...]" parses as an array literal rather
 * than a bracketed variable name.
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

/*
 * Compile a script from its model's JSON text. The script keeps no model; bsScriptToModel
 * re-parses its lines, if it has them. "scriptName" overrides the model's own name; NULL keeps it.
 * Returns NULL, with "error" set to the JSON or model error, if the text is not a script model.
 */
BSScript *bsScriptFromModelJSON(const char *text, size_t size, const char *scriptName, const char **error);

/*
 * Release a parsed script's model. The model is several times the size of the script text and
 * only the linter and coverage reporting read it; bsScriptToModel re-parses the script's retained
 * lines when it is needed after all.
 */
void bsScriptForgetModel(BSScript *script);

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
