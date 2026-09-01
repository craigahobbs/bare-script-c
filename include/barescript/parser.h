/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The BareScript parser
 *
 * The parser is a native, single-pass recursive-descent parser that produces a compiled abstract
 * syntax tree rather than the JSON "BareScript model" the JavaScript and Python implementations
 * build. Structured statements (if/while/for/break/continue) are lowered to labels and jumps
 * exactly as the reference parser lowers them, so the runtime only ever sees the six primitive
 * statement kinds. Jump labels and function-local variable slots resolve at parse time.
 *
 * See runtime.h for model conversion, which reproduces the reference JSON model.
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
 * Parse a BareScript expression. Returns the parsed expression, or NULL on error, in which case
 * "error" is filled in and must be freed with bsParserErrorFree. If "arrayLiterals" is true,
 * "[...]" parses as an array literal rather than a bracketed variable name.
 */
BSExpr *bsParseExpression(const char *text, size_t size, int lineNumber, const char *scriptName,
                          bool arrayLiterals, BSParserError *error);


/* Script reference counting */
BSScript *bsScriptRetain(BSScript *script);
void bsScriptRelease(BSScript *script);

/* Free a standalone expression parsed with bsParseExpression */
void bsExprFree(BSExpr *expr);


#ifdef __cplusplus
}
#endif

#endif
