/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The BareScript parser and the compiled script representation
 *
 * Parsing is done by barescriptParser.bare, an include library script that runs on this runtime and
 * produces the JSON "BareScript model" - so the parser, and the exact syntax and error messages it
 * accepts, are shared with the JavaScript and Python implementations. The model is converted to the
 * compiled representation below, which is what the evaluator walks: statements and expressions as C
 * structs, with a jump's label resolved to a statement index and a function-local variable resolved
 * to a slot index.
 *
 * Structured statements - if/elif/else, while, for, break, continue - never reach the runtime; the
 * parser lowers them to labels and jumps.
 */

#ifndef BARESCRIPT_PARSER_H
#define BARESCRIPT_PARSER_H

#include "value.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef struct BSExpr BSExpr;
typedef struct BSStatement BSStatement;
typedef struct BSScript BSScript;
typedef struct BSFunctionDef BSFunctionDef;


/* The expression kinds */
typedef enum {
    BS_EXPR_NUMBER,
    BS_EXPR_STRING,
    BS_EXPR_VARIABLE,
    BS_EXPR_FUNCTION,
    BS_EXPR_BINARY,
    BS_EXPR_UNARY,
    BS_EXPR_GROUP
} BSExprType;


/* The binary expression operators */
typedef enum {
    BS_BINARY_EXP,    /* ** */
    BS_BINARY_MUL,    /* *  */
    BS_BINARY_DIV,    /* /  */
    BS_BINARY_MOD,    /* %  */
    BS_BINARY_ADD,    /* +  */
    BS_BINARY_SUB,    /* -  */
    BS_BINARY_SHL,    /* << */
    BS_BINARY_SHR,    /* >> */
    BS_BINARY_LTE,    /* <= */
    BS_BINARY_LT,     /* <  */
    BS_BINARY_GTE,    /* >= */
    BS_BINARY_GT,     /* >  */
    BS_BINARY_EQ,     /* == */
    BS_BINARY_NE,     /* != */
    BS_BINARY_AND,    /* &  */
    BS_BINARY_XOR,    /* ^  */
    BS_BINARY_OR,     /* |  */
    BS_BINARY_LAND,   /* && */
    BS_BINARY_LOR,    /* || */
    BS_BINARY_COUNT
} BSBinaryOp;


/* The unary expression operators */
typedef enum {
    BS_UNARY_NEG,  /* - */
    BS_UNARY_NOT,  /* ! */
    BS_UNARY_BNOT, /* ~ */
    BS_UNARY_COUNT
} BSUnaryOp;


/* The special variable kinds - "null", "true", and "false" are not overridable */
typedef enum {
    BS_SPECIAL_NONE = 0,
    BS_SPECIAL_NULL,
    BS_SPECIAL_TRUE,
    BS_SPECIAL_FALSE
} BSSpecialVariable;


/* An expression */
struct BSExpr {
    BSExprType type;
    unsigned char pure;           /* 1 if evaluating this cannot reassign a name */
    unsigned char laterEffectful; /* bit i set if some call argument after i is not pure (i < 8) */
    union {
        double number;
        BSValue string;
        struct {
            BSValue name;
            int slot;                 /* the function-local slot index, or -1 */
            BSSpecialVariable special;
        } variable;
        struct {
            BSValue name;
            int slot;                 /* the function-local slot index, or -1 */
            bool isIf;                /* the built-in "if" function */
            BSExpr **args;
            size_t argCount;
            /*
             * Call-site cache of the last globals lookup. Valid while cachedObject still is the
             * globals object and cachedGen matches its mutation generation. A borrowed value.
             */
            BSValue cached;
            uint32_t cachedGen;
            BSObject *cachedObject;
        } function;
        struct {
            BSBinaryOp op;
            BSExpr *left;
            BSExpr *right;
        } binary;
        struct {
            BSUnaryOp op;
            BSExpr *expr;
        } unary;
        BSExpr *group;
    } u;
};


/* The statement kinds */
typedef enum {
    BS_STMT_EXPR,
    BS_STMT_JUMP,
    BS_STMT_RETURN,
    BS_STMT_LABEL,
    BS_STMT_FUNCTION,
    BS_STMT_INCLUDE
} BSStatementType;


/* An include statement's script reference */
typedef struct BSInclude {
    BSValue url;
    bool system;
} BSInclude;


/* A statement */
struct BSStatement {
    BSStatementType type;
    int lineNumber;
    int lineCount;
    union {
        struct {
            BSValue name;    /* the assignment variable name, or a null value */
            int slot;        /* the function-local slot index, or -1 */
            BSExpr *expr;
        } expr;
        struct {
            BSValue label;
            int index;       /* the resolved statement index, or -1 if unresolved */
            BSExpr *expr;    /* the jumpif test expression, or NULL */
        } jump;
        struct {
            BSExpr *expr;    /* the return expression, or NULL */
        } ret;
        struct {
            BSValue name;
        } label;
        struct {
            BSFunctionDef *def;
        } function;
        struct {
            BSInclude *includes;
            size_t count;
        } include;
    } u;
};


/* A function definition */
struct BSFunctionDef {
    BSValue name;
    BSValue *argNames;      /* the declared argument names */
    size_t argCount;
    bool lastArgArray;
    bool async;
    BSStatement **statements;
    size_t statementCount;
    BSValue *slotNames;     /* the function-local variable slot names */
    size_t slotCount;
    BSScript *script;       /* the defining script - borrowed */
    int lineNumber;
    int lineCount;
};


/* A parsed script */
struct BSScript {
    int32_t refcount;
    BSValue scriptName;     /* a string value, or a null value */
    BSValue scriptLines;    /* an array of the script's line strings */
    BSStatement **statements;
    size_t statementCount;
    BSFunctionDef **functions;
    size_t functionCount;
    bool system;            /* true if this is a system include script */

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

/* Convert a compiled statement to its JSON "ScriptStatement" model - returns an owned object value */
BSValue bsStatementToModel(const BSStatement *statement);

/* Convert a compiled expression to its JSON "Expression" model - returns an owned object value */
BSValue bsExprToModel(const BSExpr *expr);


/* Script reference counting */
BSScript *bsScriptRetain(BSScript *script);
void bsScriptRelease(BSScript *script);

/* Free a standalone expression parsed with bsParseExpression */
void bsExprFree(BSExpr *expr);


#ifdef __cplusplus
}
#endif

#endif
