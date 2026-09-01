/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The BareScript parser
 *
 * Statements are parsed a line at a time. Structured statements - if/elif/else, while, for, break,
 * and continue - are lowered to label and jump statements exactly as the reference parser lowers
 * them, including the generated "__barescript*" label and temporary variable names, so a compiled
 * script is statement-for-statement identical to the reference implementations' script model.
 *
 * Expressions are parsed by precedence climbing, which produces the same tree as the reference
 * parser's operator re-ordering pass. After a statement list is complete, two resolution passes
 * run: jump labels resolve to statement indexes, and function-local variable names resolve to
 * slot indexes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "barescript/parser.h"

#include "internal.h"


/* Parser error formatting constants */
#define BS_ERROR_LINE_MAX 120


/*
 * Character helpers
 */


static bool bsIsSpace(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == 0x0B;
}


static bool bsIsIdentBegin(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}


static bool bsIsIdent(char ch)
{
    return bsIsIdentBegin(ch) || (ch >= '0' && ch <= '9');
}


static bool bsIsDigit(char ch)
{
    return ch >= '0' && ch <= '9';
}


static size_t bsSkipSpace(const char *text, size_t size, size_t offset)
{
    while (offset < size && bsIsSpace(text[offset])) {
        offset++;
    }
    return offset;
}


/* Match an identifier; returns its size, or zero if there is none */
static size_t bsMatchIdent(const char *text, size_t size, size_t offset)
{
    if (offset >= size || !bsIsIdentBegin(text[offset])) {
        return 0;
    }
    size_t end = offset + 1;
    while (end < size && bsIsIdent(text[end])) {
        end++;
    }
    return end - offset;
}


/* Match the trailing "\s*(#.*)?$" of a statement line */
static bool bsMatchComment(const char *text, size_t size, size_t offset)
{
    offset = bsSkipSpace(text, size, offset);
    return offset == size || text[offset] == '#';
}


/* Match a literal keyword that is not followed by an identifier character */
static bool bsMatchKeyword(const char *text, size_t size, size_t offset, const char *keyword, size_t *end)
{
    size_t keywordSize = strlen(keyword);
    if (size - offset < keywordSize || memcmp(text + offset, keyword, keywordSize) != 0) {
        return false;
    }
    if (offset + keywordSize < size && bsIsIdent(text[offset + keywordSize])) {
        return false;
    }
    *end = offset + keywordSize;
    return true;
}


/*
 * Expression parsing
 */


typedef struct BSExprParser {
    const char *text;
    size_t size;
    size_t offset;
    bool arrayLiterals;
    const char *error;
    size_t errorOffset;

    /* Every allocated node, so a failed parse can free the partially built tree in one pass */
    BSExpr **nodes;
    size_t nodeCount;
    size_t nodeCapacity;
} BSExprParser;


static BSExpr *bsExprNew(BSExprParser *parser, BSExprType type)
{
    if (parser->nodeCount == parser->nodeCapacity) {
        parser->nodeCapacity = parser->nodeCapacity != 0 ? parser->nodeCapacity * 2 : 16;
        parser->nodes = bsRealloc(parser->nodes, parser->nodeCapacity * sizeof(BSExpr *));
    }
    BSExpr *expr = bsAlloc(sizeof(BSExpr));
    memset(expr, 0, sizeof(BSExpr));
    expr->type = type;
    parser->nodes[parser->nodeCount++] = expr;
    return expr;
}


static void bsExprNodeFree(BSExpr *expr)
{
    switch (expr->type) {
    case BS_EXPR_STRING:
        bsRelease(expr->u.string);
        break;
    case BS_EXPR_VARIABLE:
        bsRelease(expr->u.variable.name);
        break;
    case BS_EXPR_FUNCTION:
        bsRelease(expr->u.function.name);
        free(expr->u.function.args);
        break;
    default:
        break;
    }
    free(expr);
}


void bsExprFree(BSExpr *expr)
{
    if (expr == NULL) {
        return;
    }
    switch (expr->type) {
    case BS_EXPR_FUNCTION:
        for (size_t ix = 0; ix < expr->u.function.argCount; ix++) {
            bsExprFree(expr->u.function.args[ix]);
        }
        break;
    case BS_EXPR_BINARY:
        bsExprFree(expr->u.binary.left);
        bsExprFree(expr->u.binary.right);
        break;
    case BS_EXPR_UNARY:
        bsExprFree(expr->u.unary.expr);
        break;
    case BS_EXPR_GROUP:
        bsExprFree(expr->u.group);
        break;
    default:
        break;
    }
    bsExprNodeFree(expr);
}


static void bsExprParserFail(BSExprParser *parser)
{
    for (size_t ix = 0; ix < parser->nodeCount; ix++) {
        bsExprNodeFree(parser->nodes[ix]);
    }
    free(parser->nodes);
    parser->nodes = NULL;
    parser->nodeCount = 0;
    parser->nodeCapacity = 0;
}


static void bsExprError(BSExprParser *parser, const char *error, size_t offset)
{
    if (parser->error == NULL) {
        parser->error = error;
        parser->errorOffset = offset;
    }
}


/* Process a string literal's escape sequences */
static BSValue bsStringEscape(const char *text, size_t size)
{
    if (memchr(text, '\\', size) == NULL) {
        return bsStringNewSize(text, size);
    }

    BSStringBuilder sb;
    bsSBInit(&sb);
    size_t ix = 0;
    while (ix < size) {
        char ch = text[ix];
        if (ch != '\\' || ix + 1 >= size) {
            bsSBAppendChar(&sb, ch);
            ix++;
            continue;
        }
        char escape = text[ix + 1];
        switch (escape) {
        case 'n':
            bsSBAppendChar(&sb, '\n');
            ix += 2;
            break;
        case 'r':
            bsSBAppendChar(&sb, '\r');
            ix += 2;
            break;
        case 't':
            bsSBAppendChar(&sb, '\t');
            ix += 2;
            break;
        case 'b':
            bsSBAppendChar(&sb, '\b');
            ix += 2;
            break;
        case 'f':
            bsSBAppendChar(&sb, '\f');
            ix += 2;
            break;
        case '\'':
        case '"':
        case '\\':
            bsSBAppendChar(&sb, escape);
            ix += 2;
            break;
        case 'u': {
            uint32_t codePoint = 0;
            size_t digits = 0;
            while (digits < 4 && ix + 2 + digits < size) {
                char digit = text[ix + 2 + digits];
                uint32_t value;
                if (digit >= '0' && digit <= '9') {
                    value = (uint32_t) (digit - '0');
                } else if (digit >= 'a' && digit <= 'f') {
                    value = (uint32_t) (digit - 'a' + 10);
                } else if (digit >= 'A' && digit <= 'F') {
                    value = (uint32_t) (digit - 'A' + 10);
                } else {
                    break;
                }
                codePoint = (codePoint << 4) | value;
                digits++;
            }
            if (digits != 4) {
                /* Not a valid "\\uXXXX" escape - the backslash stays literal */
                bsSBAppendChar(&sb, ch);
                ix++;
                break;
            }
            char utf8[4];
            bsSBAppend(&sb, utf8, bsUTF8Encode(codePoint, utf8));
            ix += 6;
            break;
        }
        default:
            /* An unrecognized escape stays literal - regular expression patterns rely on this */
            bsSBAppendChar(&sb, ch);
            ix++;
            break;
        }
    }
    return bsSBToValue(&sb);
}


/* The binary operator precedence levels - higher binds tighter */
static int bsBinaryPrecedence(BSBinaryOp op)
{
    switch (op) {
    case BS_BINARY_EXP:
        return 10;
    case BS_BINARY_MUL:
    case BS_BINARY_DIV:
    case BS_BINARY_MOD:
        return 9;
    case BS_BINARY_ADD:
    case BS_BINARY_SUB:
        return 8;
    case BS_BINARY_SHL:
    case BS_BINARY_SHR:
        return 7;
    case BS_BINARY_LTE:
    case BS_BINARY_LT:
    case BS_BINARY_GTE:
    case BS_BINARY_GT:
        return 6;
    case BS_BINARY_EQ:
    case BS_BINARY_NE:
        return 5;
    case BS_BINARY_AND:
        return 4;
    case BS_BINARY_XOR:
        return 3;
    case BS_BINARY_OR:
        return 2;
    case BS_BINARY_LAND:
        return 1;
    default:
        return 0;
    }
}


const char *bsBinaryOpText[BS_BINARY_COUNT] = {
    "**", "*", "/", "%", "+", "-", "<<", ">>", "<=", "<", ">=", ">", "==", "!=", "&", "^", "|", "&&", "||"
};

const char *bsUnaryOpText[BS_UNARY_COUNT] = {"-", "!", "~"};


/*
 * Match a binary operator. Returns true if an operator was matched, setting "*op" and advancing
 * "*offset". Sets "*done" if the expression ended - end of text or a comment.
 */
static bool bsMatchBinaryOp(const char *text, size_t size, size_t *offset, BSBinaryOp *op, bool *done)
{
    size_t ix = bsSkipSpace(text, size, *offset);
    *done = false;
    if (ix >= size || text[ix] == '#') {
        *done = true;
        return false;
    }

    /* The operator alternatives are ordered longest-first, matching the reference parser's regex */
    static const BSBinaryOp order[] = {
        BS_BINARY_EXP, BS_BINARY_MUL, BS_BINARY_DIV, BS_BINARY_MOD, BS_BINARY_ADD, BS_BINARY_SUB,
        BS_BINARY_SHL, BS_BINARY_SHR, BS_BINARY_LTE, BS_BINARY_LT, BS_BINARY_GTE, BS_BINARY_GT,
        BS_BINARY_EQ, BS_BINARY_NE, BS_BINARY_LAND, BS_BINARY_LOR, BS_BINARY_AND, BS_BINARY_XOR,
        BS_BINARY_OR
    };
    for (size_t ixOp = 0; ixOp < sizeof(order) / sizeof(order[0]); ixOp++) {
        const char *opText = bsBinaryOpText[order[ixOp]];
        size_t opSize = strlen(opText);
        if (size - ix >= opSize && memcmp(text + ix, opText, opSize) == 0) {
            *op = order[ixOp];
            *offset = ix + opSize;
            return true;
        }
    }
    return false;
}


static BSExpr *bsParseBinary(BSExprParser *parser, int minPrecedence);


static BSExpr *bsParseUnary(BSExprParser *parser)
{
    const char *text = parser->text;
    size_t size = parser->size;
    size_t ix = bsSkipSpace(text, size, parser->offset);
    size_t tokenBegin = parser->offset;

    if (ix >= size) {
        bsExprError(parser, "Syntax error", tokenBegin);
        return NULL;
    }

    /* A variable or function call */
    size_t identSize = bsMatchIdent(text, size, ix);
    if (identSize != 0) {
        BSValue name = bsStringNewSize(text + ix, identSize);
        size_t after = bsSkipSpace(text, size, ix + identSize);

        /* Not a function call? */
        if (after >= size || text[after] != '(') {
            parser->offset = ix + identSize;
            BSExpr *expr = bsExprNew(parser, BS_EXPR_VARIABLE);
            expr->u.variable.name = name;
            expr->u.variable.slot = -1;
            if (bsStringSize(name) == 4 && memcmp(bsStringData(name), "null", 4) == 0) {
                expr->u.variable.special = BS_SPECIAL_NULL;
            } else if (bsStringSize(name) == 4 && memcmp(bsStringData(name), "true", 4) == 0) {
                expr->u.variable.special = BS_SPECIAL_TRUE;
            } else if (bsStringSize(name) == 5 && memcmp(bsStringData(name), "false", 5) == 0) {
                expr->u.variable.special = BS_SPECIAL_FALSE;
            }
            return expr;
        }

        /* A function call - parse the arguments */
        parser->offset = after + 1;
        BSExpr *expr = bsExprNew(parser, BS_EXPR_FUNCTION);
        expr->u.function.name = name;
        expr->u.function.slot = -1;
        expr->u.function.isIf = (bsStringSize(name) == 2 && memcmp(bsStringData(name), "if", 2) == 0);

        size_t argCapacity = 0;
        while (true) {
            size_t argIx = bsSkipSpace(text, size, parser->offset);
            if (argIx < size && text[argIx] == ')') {
                parser->offset = argIx + 1;
                break;
            }
            if (expr->u.function.argCount != 0) {
                if (argIx >= size || text[argIx] != ',') {
                    bsExprError(parser, "Syntax error", parser->offset);
                    return NULL;
                }
                parser->offset = argIx + 1;
            }
            BSExpr *arg = bsParseBinary(parser, 0);
            if (arg == NULL) {
                return NULL;
            }
            if (expr->u.function.argCount == argCapacity) {
                argCapacity = argCapacity != 0 ? argCapacity * 2 : 4;
                expr->u.function.args = bsRealloc(expr->u.function.args, argCapacity * sizeof(BSExpr *));
            }
            expr->u.function.args[expr->u.function.argCount++] = arg;
        }
        return expr;
    }

    /* A string literal */
    char quote = text[ix];
    if (quote == '\'' || quote == '"') {
        size_t begin = ix + 1;
        size_t end = begin;
        while (end < size && text[end] != quote) {
            /* "\\\\" and an escaped quote are the only two-character sequences the token consumes */
            if (text[end] == '\\' && end + 1 < size && (text[end + 1] == '\\' || text[end + 1] == quote)) {
                end += 2;
            } else {
                end++;
            }
        }
        if (end >= size) {
            bsExprError(parser, "Syntax error", tokenBegin);
            return NULL;
        }
        parser->offset = end + 1;
        BSExpr *expr = bsExprNew(parser, BS_EXPR_STRING);
        expr->u.string = bsStringEscape(text + begin, end - begin);
        return expr;
    }

    /* A number literal */
    if (bsIsDigit(text[ix]) ||
        ((text[ix] == '+' || text[ix] == '-') && ix + 1 < size && bsIsDigit(text[ix + 1]))) {
        double number;
        if (size - ix > 2 && text[ix] == '0' && text[ix + 1] == 'x') {
            size_t end = ix + 2;
            while (end < size && ((text[end] >= '0' && text[end] <= '9') ||
                                  (text[end] >= 'a' && text[end] <= 'f') ||
                                  (text[end] >= 'A' && text[end] <= 'F'))) {
                end++;
            }
            if (end != ix + 2) {
                bsIntegerParse(text + ix + 2, end - ix - 2, 16, &number);
                parser->offset = end;
                BSExpr *expr = bsExprNew(parser, BS_EXPR_NUMBER);
                expr->u.number = number;
                return expr;
            }
        }

        /* "[+-]?[0-9]+(?:\.[0-9]*)?(?:e[+-]?[0-9]+)?" - note the lower-case exponent */
        size_t end = ix;
        if (text[end] == '+' || text[end] == '-') {
            end++;
        }
        while (end < size && bsIsDigit(text[end])) {
            end++;
        }
        if (end < size && text[end] == '.') {
            end++;
            while (end < size && bsIsDigit(text[end])) {
                end++;
            }
        }
        if (end < size && text[end] == 'e') {
            size_t save = end;
            end++;
            if (end < size && (text[end] == '+' || text[end] == '-')) {
                end++;
            }
            size_t digits = end;
            while (end < size && bsIsDigit(text[end])) {
                end++;
            }
            if (end == digits) {
                end = save;
            }
        }
        bsNumberParse(text + ix, end - ix, &number);
        parser->offset = end;
        BSExpr *expr = bsExprNew(parser, BS_EXPR_NUMBER);
        expr->u.number = number;
        return expr;
    }

    /* A unary operator */
    char punc = text[ix];
    if (punc == '!' || punc == '-' || punc == '~') {
        parser->offset = ix + 1;
        BSExpr *operand = bsParseUnary(parser);
        if (operand == NULL) {
            return NULL;
        }
        BSExpr *expr = bsExprNew(parser, BS_EXPR_UNARY);
        expr->u.unary.op = (punc == '!' ? BS_UNARY_NOT : (punc == '-' ? BS_UNARY_NEG : BS_UNARY_BNOT));
        expr->u.unary.expr = operand;
        return expr;
    }

    /* A group */
    if (punc == '(') {
        parser->offset = ix + 1;
        BSExpr *inner = bsParseBinary(parser, 0);
        if (inner == NULL) {
            return NULL;
        }
        size_t closeIx = bsSkipSpace(text, size, parser->offset);
        if (closeIx >= size || text[closeIx] != ')') {
            bsExprError(parser, "Unmatched parenthesis", tokenBegin);
            return NULL;
        }
        parser->offset = closeIx + 1;
        BSExpr *expr = bsExprNew(parser, BS_EXPR_GROUP);
        expr->u.group = inner;
        return expr;
    }

    /* An object literal - lowered to an objectNew call of alternating key and value arguments */
    if (punc == '{' || (punc == '[' && parser->arrayLiterals)) {
        bool isObject = (punc == '{');
        char close = isObject ? '}' : ']';
        parser->offset = ix + 1;
        BSExpr *expr = bsExprNew(parser, BS_EXPR_FUNCTION);
        expr->u.function.name = bsStringNew(isObject ? "objectNew" : "arrayNew");
        expr->u.function.slot = -1;
        size_t argCapacity = 0;
        while (true) {
            size_t argIx = bsSkipSpace(text, size, parser->offset);
            if (argIx < size && text[argIx] == close) {
                parser->offset = argIx + 1;
                break;
            }
            if (expr->u.function.argCount != 0) {
                if (argIx >= size || text[argIx] != ',') {
                    bsExprError(parser, "Syntax error", parser->offset);
                    return NULL;
                }
                parser->offset = argIx + 1;
            }
            for (int ixPart = 0; ixPart < (isObject ? 2 : 1); ixPart++) {
                if (ixPart == 1) {
                    size_t colonIx = bsSkipSpace(text, size, parser->offset);
                    if (colonIx >= size || text[colonIx] != ':') {
                        bsExprError(parser, "Syntax error", parser->offset);
                        return NULL;
                    }
                    parser->offset = colonIx + 1;
                }
                BSExpr *arg = bsParseBinary(parser, 0);
                if (arg == NULL) {
                    return NULL;
                }
                if (expr->u.function.argCount == argCapacity) {
                    argCapacity = argCapacity != 0 ? argCapacity * 2 : 4;
                    expr->u.function.args = bsRealloc(expr->u.function.args, argCapacity * sizeof(BSExpr *));
                }
                expr->u.function.args[expr->u.function.argCount++] = arg;
            }
        }
        return expr;
    }

    /* A bracketed variable name - only outside of scripts, where "[" is an array literal */
    if (punc == '[') {
        size_t begin = bsSkipSpace(text, size, ix + 1);
        size_t end = begin;
        while (end < size && text[end] != ']') {
            if (text[end] == '\\' && end + 1 < size) {
                end += 2;
            } else {
                end++;
            }
        }
        if (end >= size || end == begin) {
            bsExprError(parser, "Syntax error", tokenBegin);
            return NULL;
        }
        size_t nameEnd = end;
        while (nameEnd > begin && bsIsSpace(text[nameEnd - 1])) {
            nameEnd--;
        }

        /* Un-escape "\\]" and "\\\\" */
        BSStringBuilder sb;
        bsSBInit(&sb);
        for (size_t ixName = begin; ixName < nameEnd; ixName++) {
            if (text[ixName] == '\\' && ixName + 1 < nameEnd &&
                (text[ixName + 1] == ']' || text[ixName + 1] == '\\')) {
                ixName++;
            }
            bsSBAppendChar(&sb, text[ixName]);
        }
        parser->offset = end + 1;
        BSExpr *expr = bsExprNew(parser, BS_EXPR_VARIABLE);
        expr->u.variable.name = bsSBToValue(&sb);
        expr->u.variable.slot = -1;
        return expr;
    }

    bsExprError(parser, "Syntax error", tokenBegin);
    return NULL;
}


static BSExpr *bsParseBinary(BSExprParser *parser, int minPrecedence)
{
    BSExpr *left = bsParseUnary(parser);
    if (left == NULL) {
        return NULL;
    }

    while (true) {
        size_t offset = parser->offset;
        BSBinaryOp op;
        bool done;
        if (!bsMatchBinaryOp(parser->text, parser->size, &offset, &op, &done)) {
            if (done) {
                parser->offset = parser->size;
            }
            return left;
        }
        int precedence = bsBinaryPrecedence(op);
        if (precedence < minPrecedence) {
            return left;
        }
        parser->offset = offset;

        /* All BareScript binary operators are left-associative */
        BSExpr *right = bsParseBinary(parser, precedence + 1);
        if (right == NULL) {
            return NULL;
        }
        BSExpr *binary = bsExprNew(parser, BS_EXPR_BINARY);
        binary->u.binary.op = op;
        binary->u.binary.left = left;
        binary->u.binary.right = right;
        left = binary;
    }
}


/*
 * Parser errors
 */


void bsParserErrorFree(BSParserError *error)
{
    bsRelease(error->error);
    bsRelease(error->line);
    bsRelease(error->scriptName);
    bsRelease(error->message);
    memset(error, 0, sizeof(*error));
}


static void bsParserErrorSet(BSParserError *error, const char *description, const char *line, size_t lineSize,
                             int columnNumber, int lineNumber, const char *scriptName)
{
    error->error = bsStringNew(description);
    error->line = bsStringNewSize(line, lineSize);
    error->scriptName = scriptName != NULL ? bsStringNew(scriptName) : bsNull();
    error->columnNumber = columnNumber;
    error->lineNumber = lineNumber;

    /* Trim the error line to a readable width, keeping the error column visible */
    BSValue lineError = bsRetain(error->line);
    int lineColumn = columnNumber;
    size_t lineLength = bsStringLength(error->line);
    if (lineLength > BS_ERROR_LINE_MAX) {
        int lineLeft = columnNumber - 1 - BS_ERROR_LINE_MAX / 2;
        int lineRight = lineLeft + BS_ERROR_LINE_MAX;
        const char *data = bsStringData(error->line);
        if (lineLeft < 0) {
            size_t end = bsStringOffset(error->line, BS_ERROR_LINE_MAX);
            bsAssign(&lineError, bsStringNewFormat("%.*s ...", (int) end, data));
        } else if ((size_t) lineRight > lineLength) {
            size_t begin = bsStringOffset(error->line, lineLength - BS_ERROR_LINE_MAX);
            bsAssign(&lineError, bsStringNewFormat("... %s", data + begin));
            lineColumn -= lineLeft - 4 - (lineRight - (int) lineLength);
        } else {
            size_t begin = bsStringOffset(error->line, (size_t) lineLeft);
            size_t end = bsStringOffset(error->line, (size_t) lineRight);
            bsAssign(&lineError, bsStringNewFormat("... %.*s ...", (int) (end - begin), data + begin));
            lineColumn -= lineLeft - 4;
        }
    }

    BSStringBuilder sb;
    bsSBInit(&sb);
    if (lineNumber != 0) {
        bsSBAppendFormat(&sb, "%s:%d: ", scriptName != NULL ? scriptName : "", lineNumber);
    }
    bsSBAppendFormat(&sb, "%s\n%s\n", description, bsStringData(lineError));
    for (int ix = 1; ix < lineColumn; ix++) {
        bsSBAppendChar(&sb, ' ');
    }
    bsSBAppendString(&sb, "^\n");
    error->message = bsSBToValue(&sb);
    bsRelease(lineError);
}


/* The error column number, in code points, of an offset into a text */
static int bsErrorColumn(const char *text, size_t offset)
{
    size_t length = bsUTF8Length(text, offset);
    return (int) (length != SIZE_MAX ? length : offset) + 1;
}


BSExpr *bsParseExpression(const char *text, size_t size, int lineNumber, const char *scriptName,
                          bool arrayLiterals, BSParserError *error)
{
    BSExprParser parser;
    memset(&parser, 0, sizeof(parser));
    parser.text = text;
    parser.size = size;
    parser.arrayLiterals = arrayLiterals;

    BSExpr *expr = bsParseBinary(&parser, 0);
    if (expr != NULL && bsSkipSpace(text, size, parser.offset) != size) {
        bsExprError(&parser, "Syntax error", parser.offset);
        expr = NULL;
    }
    if (expr == NULL) {
        if (error != NULL) {
            bsParserErrorSet(error, parser.error, text, size, bsErrorColumn(text, parser.errorOffset),
                             lineNumber, scriptName);
        }
        bsExprParserFail(&parser);
        return NULL;
    }
    free(parser.nodes);
    return expr;
}


/*
 * Statement parsing
 */


typedef struct BSStatementList {
    BSStatement **statements;
    size_t count;
    size_t capacity;
} BSStatementList;


static BSStatement *bsStatementAdd(BSStatementList *list, BSStatementType type, int lineNumber, int lineCount)
{
    if (list->count == list->capacity) {
        list->capacity = list->capacity != 0 ? list->capacity * 2 : 16;
        list->statements = bsRealloc(list->statements, list->capacity * sizeof(BSStatement *));
    }
    BSStatement *statement = bsAlloc(sizeof(BSStatement));
    memset(statement, 0, sizeof(BSStatement));
    statement->type = type;
    statement->lineNumber = lineNumber;
    statement->lineCount = lineCount;
    if (type == BS_STMT_EXPR) {
        statement->u.expr.name = bsNull();
        statement->u.expr.slot = -1;
    } else if (type == BS_STMT_JUMP) {
        statement->u.jump.label = bsNull();
        statement->u.jump.index = -1;
    } else if (type == BS_STMT_LABEL) {
        statement->u.label.name = bsNull();
    }
    list->statements[list->count++] = statement;
    return statement;
}


static void bsStatementFree(BSStatement *statement)
{
    switch (statement->type) {
    case BS_STMT_EXPR:
        bsRelease(statement->u.expr.name);
        bsExprFree(statement->u.expr.expr);
        break;
    case BS_STMT_JUMP:
        bsRelease(statement->u.jump.label);
        bsExprFree(statement->u.jump.expr);
        break;
    case BS_STMT_RETURN:
        bsExprFree(statement->u.ret.expr);
        break;
    case BS_STMT_LABEL:
        bsRelease(statement->u.label.name);
        break;
    case BS_STMT_FUNCTION:
        break;
    default:
        for (size_t ix = 0; ix < statement->u.include.count; ix++) {
            bsRelease(statement->u.include.includes[ix].url);
        }
        free(statement->u.include.includes);
        break;
    }
    free(statement);
}


void bsStatementsFree(BSStatement **statements, size_t statementCount)
{
    for (size_t ix = 0; ix < statementCount; ix++) {
        bsStatementFree(statements[ix]);
    }
    free(statements);
}


void bsFunctionDefFree(BSFunctionDef *def)
{
    bsRelease(def->name);
    for (size_t ix = 0; ix < def->argCount; ix++) {
        bsRelease(def->argNames[ix]);
    }
    free(def->argNames);
    for (size_t ix = 0; ix < def->slotCount; ix++) {
        bsRelease(def->slotNames[ix]);
    }
    free(def->slotNames);
    bsStatementsFree(def->statements, def->statementCount);
    free(def);
}


/* The parser's structured statement stack entry */
typedef enum {
    BS_LABEL_IF,
    BS_LABEL_WHILE,
    BS_LABEL_FOR
} BSLabelDefType;

typedef struct BSLabelDef {
    BSLabelDefType type;
    BSStatement *jump;   /* the if-then jump statement */
    BSValue done;
    BSValue loop;
    BSValue continueLabel;
    BSExpr *expr;        /* the while-do test expression - copied for the footer jump */
    BSValue indexName;   /* the foreach loop's index variable name */
    BSValue valuesName;  /* the foreach loop's values variable name */
    BSValue lengthName;  /* the foreach loop's length variable name */
    BSValue valueName;   /* the foreach loop's value variable name */
    bool hasElse;
    bool hasContinue;
    BSValue line;
    int lineNumber;
} BSLabelDef;


typedef struct BSScriptParser {
    BSScript *script;
    BSStatementList top;
    BSStatementList *statements;
    BSLabelDef *labelDefs;
    size_t labelDefCount;
    size_t labelDefCapacity;
    size_t functionLabelDefDepth;
    BSFunctionDef *functionDef;
    BSStatementList functionStatements;
    int labelIndex;
    int startLineNumber;
    const char *scriptName;
    BSParserError *error;
    bool failed;
} BSScriptParser;


static void bsScriptParserError(BSScriptParser *parser, const char *description, const char *line,
                                size_t lineSize, int columnNumber, int lineNumber)
{
    if (!parser->failed) {
        parser->failed = true;
        if (parser->error != NULL) {
            bsParserErrorSet(parser->error, description, line, lineSize, columnNumber, lineNumber,
                             parser->scriptName);
        }
    }
}


static BSValue bsGeneratedLabel(const char *prefix, int index)
{
    return bsStringNewFormat("%s%d", prefix, index);
}


static void bsLabelDefPush(BSScriptParser *parser, const BSLabelDef *def)
{
    if (parser->labelDefCount == parser->labelDefCapacity) {
        parser->labelDefCapacity = parser->labelDefCapacity != 0 ? parser->labelDefCapacity * 2 : 8;
        parser->labelDefs = bsRealloc(parser->labelDefs, parser->labelDefCapacity * sizeof(BSLabelDef));
    }
    parser->labelDefs[parser->labelDefCount++] = *def;
}


static void bsLabelDefFree(BSLabelDef *def)
{
    bsRelease(def->done);
    bsRelease(def->loop);
    bsRelease(def->continueLabel);
    bsRelease(def->indexName);
    bsRelease(def->valuesName);
    bsRelease(def->lengthName);
    bsRelease(def->valueName);
    bsRelease(def->line);
    bsExprFree(def->expr);
}


/* Parse a line's expression, reporting errors against the statement line */
static BSExpr *bsParseLineExpression(BSScriptParser *parser, const char *exprText, size_t exprSize,
                                     const char *line, size_t lineSize, size_t exprColumnOffset,
                                     int lineNumber)
{
    BSParserError exprError;
    memset(&exprError, 0, sizeof(exprError));
    BSExpr *expr = bsParseExpression(exprText, exprSize, lineNumber, parser->scriptName, true, &exprError);
    if (expr == NULL) {
        int columnNumber = (int) exprColumnOffset + exprError.columnNumber;
        bsScriptParserError(parser, bsStringData(exprError.error), line, lineSize, columnNumber, lineNumber);
        bsParserErrorFree(&exprError);
    }
    return expr;
}



/* Deep-copy an expression - the while-do footer jump reuses the loop test expression */
static BSExpr *bsExprCopy(const BSExpr *expr)
{
    BSExpr *copy = bsAlloc(sizeof(BSExpr));
    memcpy(copy, expr, sizeof(BSExpr));
    switch (expr->type) {
    case BS_EXPR_STRING:
        bsRetain(copy->u.string);
        break;
    case BS_EXPR_VARIABLE:
        bsRetain(copy->u.variable.name);
        break;
    case BS_EXPR_FUNCTION:
        bsRetain(copy->u.function.name);
        if (expr->u.function.argCount != 0) {
            copy->u.function.args = bsAlloc(expr->u.function.argCount * sizeof(BSExpr *));
            for (size_t ix = 0; ix < expr->u.function.argCount; ix++) {
                copy->u.function.args[ix] = bsExprCopy(expr->u.function.args[ix]);
            }
        } else {
            copy->u.function.args = NULL;
        }
        break;
    case BS_EXPR_BINARY:
        copy->u.binary.left = bsExprCopy(expr->u.binary.left);
        copy->u.binary.right = bsExprCopy(expr->u.binary.right);
        break;
    case BS_EXPR_UNARY:
        copy->u.unary.expr = bsExprCopy(expr->u.unary.expr);
        break;
    case BS_EXPR_GROUP:
        copy->u.group = bsExprCopy(expr->u.group);
        break;
    default:
        break;
    }
    return copy;
}


/* Create a variable expression */
static BSExpr *bsExprVariable(BSValue name)
{
    BSExpr *expr = bsAlloc(sizeof(BSExpr));
    memset(expr, 0, sizeof(BSExpr));
    expr->type = BS_EXPR_VARIABLE;
    expr->u.variable.name = bsRetain(name);
    expr->u.variable.slot = -1;
    return expr;
}


/* Create a number expression */
static BSExpr *bsExprNumber(double number)
{
    BSExpr *expr = bsAlloc(sizeof(BSExpr));
    memset(expr, 0, sizeof(BSExpr));
    expr->type = BS_EXPR_NUMBER;
    expr->u.number = number;
    return expr;
}


/* Create a unary expression */
static BSExpr *bsExprUnary(BSUnaryOp op, BSExpr *operand)
{
    BSExpr *expr = bsAlloc(sizeof(BSExpr));
    memset(expr, 0, sizeof(BSExpr));
    expr->type = BS_EXPR_UNARY;
    expr->u.unary.op = op;
    expr->u.unary.expr = operand;
    return expr;
}


/* Create a binary expression */
static BSExpr *bsExprBinary(BSBinaryOp op, BSExpr *left, BSExpr *right)
{
    BSExpr *expr = bsAlloc(sizeof(BSExpr));
    memset(expr, 0, sizeof(BSExpr));
    expr->type = BS_EXPR_BINARY;
    expr->u.binary.op = op;
    expr->u.binary.left = left;
    expr->u.binary.right = right;
    return expr;
}


/* Create a function call expression of the given arguments */
static BSExpr *bsExprCall(const char *name, BSExpr **args, size_t argCount)
{
    BSExpr *expr = bsAlloc(sizeof(BSExpr));
    memset(expr, 0, sizeof(BSExpr));
    expr->type = BS_EXPR_FUNCTION;
    expr->u.function.name = bsStringNew(name);
    expr->u.function.slot = -1;
    if (argCount != 0) {
        expr->u.function.args = bsAlloc(argCount * sizeof(BSExpr *));
        memcpy(expr->u.function.args, args, argCount * sizeof(BSExpr *));
        expr->u.function.argCount = argCount;
    }
    return expr;
}


/*
 * Slot resolution
 *
 * A function's local variables are its declared arguments plus every assignment target in its
 * body - a statically known set, since BareScript has no dynamic local creation. Resolving each
 * name to a slot index turns a variable read into an array load.
 */


typedef struct BSSlotMap {
    BSValue *names;
    size_t count;
    size_t capacity;
} BSSlotMap;


static int bsSlotFind(const BSSlotMap *map, BSValue name)
{
    for (size_t ix = 0; ix < map->count; ix++) {
        if (bsValueCompare(map->names[ix], name) == 0) {
            return (int) ix;
        }
    }
    return -1;
}


static void bsSlotAdd(BSSlotMap *map, BSValue name)
{
    if (bsSlotFind(map, name) >= 0) {
        return;
    }
    if (map->count == map->capacity) {
        map->capacity = map->capacity != 0 ? map->capacity * 2 : 16;
        map->names = bsRealloc(map->names, map->capacity * sizeof(BSValue));
    }
    map->names[map->count++] = bsRetain(name);
}


static void bsResolveExprSlots(BSExpr *expr, const BSSlotMap *map)
{
    switch (expr->type) {
    case BS_EXPR_VARIABLE:
        expr->u.variable.slot = bsSlotFind(map, expr->u.variable.name);
        break;
    case BS_EXPR_FUNCTION:
        expr->u.function.slot = bsSlotFind(map, expr->u.function.name);
        for (size_t ix = 0; ix < expr->u.function.argCount; ix++) {
            bsResolveExprSlots(expr->u.function.args[ix], map);
        }
        break;
    case BS_EXPR_BINARY:
        bsResolveExprSlots(expr->u.binary.left, map);
        bsResolveExprSlots(expr->u.binary.right, map);
        break;
    case BS_EXPR_UNARY:
        bsResolveExprSlots(expr->u.unary.expr, map);
        break;
    case BS_EXPR_GROUP:
        bsResolveExprSlots(expr->u.group, map);
        break;
    default:
        break;
    }
}


static void bsResolveSlots(BSFunctionDef *def)
{
    BSSlotMap map;
    memset(&map, 0, sizeof(map));

    /* The declared arguments come first, so an argument's slot is its position */
    for (size_t ix = 0; ix < def->argCount; ix++) {
        bsSlotAdd(&map, def->argNames[ix]);
    }
    for (size_t ix = 0; ix < def->statementCount; ix++) {
        BSStatement *statement = def->statements[ix];
        if (statement->type == BS_STMT_EXPR && statement->u.expr.name.type == BS_STRING) {
            bsSlotAdd(&map, statement->u.expr.name);
        }
    }

    for (size_t ix = 0; ix < def->statementCount; ix++) {
        BSStatement *statement = def->statements[ix];
        switch (statement->type) {
        case BS_STMT_EXPR:
            if (statement->u.expr.name.type == BS_STRING) {
                statement->u.expr.slot = bsSlotFind(&map, statement->u.expr.name);
            }
            bsResolveExprSlots(statement->u.expr.expr, &map);
            break;
        case BS_STMT_JUMP:
            if (statement->u.jump.expr != NULL) {
                bsResolveExprSlots(statement->u.jump.expr, &map);
            }
            break;
        case BS_STMT_RETURN:
            if (statement->u.ret.expr != NULL) {
                bsResolveExprSlots(statement->u.ret.expr, &map);
            }
            break;
        default:
            break;
        }
    }

    def->slotNames = map.names;
    def->slotCount = map.count;
}


/*
 * Jump resolution - a jump label resolves to its statement index
 */


static void bsResolveJumps(BSStatement **statements, size_t statementCount)
{
    BSValue labels = bsObjectNew();
    for (size_t ix = 0; ix < statementCount; ix++) {
        if (statements[ix]->type == BS_STMT_LABEL) {
            bsObjectSetString(labels, statements[ix]->u.label.name, bsNumber((double) ix));
        }
    }
    for (size_t ix = 0; ix < statementCount; ix++) {
        BSStatement *statement = statements[ix];
        if (statement->type == BS_STMT_JUMP) {
            BSValue index = bsObjectGetString(labels, statement->u.jump.label);
            statement->u.jump.index = index.type == BS_NUMBER ? (int) index.u.number : -1;
        }
    }
    bsRelease(labels);
}


/*
 * The script parser
 */


BSScript *bsScriptRetain(BSScript *script)
{
    script->refcount++;
    return script;
}


void bsScriptRelease(BSScript *script)
{
    if (script == NULL || --script->refcount != 0) {
        return;
    }
    bsStatementsFree(script->statements, script->statementCount);
    for (size_t ix = 0; ix < script->functionCount; ix++) {
        bsFunctionDefFree(script->functions[ix]);
    }
    free(script->functions);
    bsRelease(script->scriptName);
    bsRelease(script->scriptLines);
    free(script);
}


static void bsScriptAddFunction(BSScript *script, BSFunctionDef *def, size_t *capacity)
{
    if (script->functionCount == *capacity) {
        *capacity = *capacity != 0 ? *capacity * 2 : 8;
        script->functions = bsRealloc(script->functions, *capacity * sizeof(BSFunctionDef *));
    }
    script->functions[script->functionCount++] = def;
}



/*
 * Statement construction helpers
 */


static BSStatement *bsAddJump(BSStatementList *list, BSValue label, BSExpr *expr, int lineNumber, int lineCount)
{
    BSStatement *statement = bsStatementAdd(list, BS_STMT_JUMP, lineNumber, lineCount);
    statement->u.jump.label = bsRetain(label);
    statement->u.jump.expr = expr;
    return statement;
}


static void bsAddLabel(BSStatementList *list, BSValue name, int lineNumber, int lineCount)
{
    BSStatement *statement = bsStatementAdd(list, BS_STMT_LABEL, lineNumber, lineCount);
    statement->u.label.name = bsRetain(name);
}


static void bsAddAssign(BSStatementList *list, BSValue name, BSExpr *expr, int lineNumber, int lineCount)
{
    BSStatement *statement = bsStatementAdd(list, BS_STMT_EXPR, lineNumber, lineCount);
    statement->u.expr.name = bsRetain(name);
    statement->u.expr.expr = expr;
}


/* The statement list the parser is appending to - a function body, or the script's top level */
static BSStatementList *bsCurrentStatements(BSScriptParser *parser)
{
    return parser->functionDef != NULL ? &parser->functionStatements : &parser->top;
}


/* The label definition stack depth below which a structured statement may not reach */
static size_t bsLabelDefDepth(const BSScriptParser *parser)
{
    return parser->functionDef != NULL ? parser->functionLabelDefDepth : 0;
}


/*
 * Match a block header's "<expr>:" tail. The reference parser's ".+" is greedy, so the last colon
 * that leaves a valid comment tail wins - which is what makes "if a == ':':" parse.
 */
static bool bsMatchBlockTail(const char *line, size_t size, size_t exprBegin, size_t *exprEnd)
{
    for (size_t ix = size; ix > exprBegin; ix--) {
        if (line[ix - 1] == ':' && bsMatchComment(line, size, ix)) {
            *exprEnd = ix - 1;
            return true;
        }
    }
    return false;
}


/*
 * Match a block header's " <expr>:" tail, emulating the reference parser's backtracking
 *
 * The header regex is "\s+(?<expr>.+)\s*:", so the whitespace run is greedy but gives characters
 * back until the expression is non-empty and a colon tail matches - which is what makes the
 * degenerate "if  :" report its error at the expression rather than failing to match at all.
 */
static bool bsMatchBlockExpr(const char *line, size_t lineSize, size_t keywordEnd, size_t *exprBegin,
                             size_t *exprEnd)
{
    size_t maxBegin = bsSkipSpace(line, lineSize, keywordEnd);
    if (maxBegin == keywordEnd) {
        return false;
    }
    for (size_t begin = maxBegin; begin > keywordEnd; begin--) {
        if (bsMatchBlockTail(line, lineSize, begin, exprEnd)) {
            *exprBegin = begin;
            return true;
        }
    }
    return false;
}


/* The code point column offset of a byte offset into a line */
static size_t bsColumnOffset(const char *line, size_t offset)
{
    size_t length = bsUTF8Length(line, offset);
    return length != SIZE_MAX ? length : offset;
}


static const char *bsStatementKeywords[] = {
    "function", "async", "endfunction", "if", "elif", "else", "endif", "while", "endwhile",
    "for", "endfor", "break", "continue", "jump", "jumpif", "return", "include"
};


static bool bsIsStatementKeyword(const char *text, size_t size)
{
    for (size_t ix = 0; ix < sizeof(bsStatementKeywords) / sizeof(bsStatementKeywords[0]); ix++) {
        if (strlen(bsStatementKeywords[ix]) == size && memcmp(bsStatementKeywords[ix], text, size) == 0) {
            return true;
        }
    }
    return false;
}


static void bsParseStatement(BSScriptParser *parser, const char *line, size_t lineSize, BSValue lineValue,
                             int lineNumber, int lineCount, int errorLineNumber, int ixLinePart,
                             size_t *functionCapacity)
{
    BSStatementList *statements = bsCurrentStatements(parser);
    size_t keywordBegin = bsSkipSpace(line, lineSize, 0);
    size_t keywordSize = bsMatchIdent(line, lineSize, keywordBegin);
    bool isKeyword = keywordSize != 0 && bsIsStatementKeyword(line + keywordBegin, keywordSize);
    size_t keywordEnd = keywordBegin + keywordSize;

    if (isKeyword) {
        const char *keyword = line + keywordBegin;

        /* A function definition */
        if ((keywordSize == 8 && memcmp(keyword, "function", 8) == 0) ||
            (keywordSize == 5 && memcmp(keyword, "async", 5) == 0)) {
            bool async = (keywordSize == 5);
            size_t ix = keywordEnd;
            bool matched = true;
            if (async) {
                ix = bsSkipSpace(line, lineSize, ix);
                if (!bsMatchKeyword(line, lineSize, ix, "function", &ix)) {
                    matched = false;
                }
            }

            BSValue name = bsNull();
            BSValue *argNames = NULL;
            size_t argCount = 0;
            size_t argCapacity = 0;
            bool lastArgArray = false;
            if (matched) {
                size_t spaceIx = bsSkipSpace(line, lineSize, ix);
                size_t nameSize = (spaceIx != ix) ? bsMatchIdent(line, lineSize, spaceIx) : 0;
                if (nameSize == 0) {
                    matched = false;
                } else {
                    name = bsStringNewSize(line + spaceIx, nameSize);
                    ix = bsSkipSpace(line, lineSize, spaceIx + nameSize);
                    if (ix >= lineSize || line[ix] != '(') {
                        matched = false;
                    } else {
                        ix = bsSkipSpace(line, lineSize, ix + 1);
                    }
                }
            }
            if (matched) {
                size_t argSize = bsMatchIdent(line, lineSize, ix);
                while (argSize != 0) {
                    if (argCount == argCapacity) {
                        argCapacity = argCapacity != 0 ? argCapacity * 2 : 8;
                        argNames = bsRealloc(argNames, argCapacity * sizeof(BSValue));
                    }
                    argNames[argCount++] = bsStringNewSize(line + ix, argSize);
                    ix += argSize;
                    size_t commaIx = bsSkipSpace(line, lineSize, ix);
                    if (commaIx >= lineSize || line[commaIx] != ',') {
                        break;
                    }
                    size_t nextIx = bsSkipSpace(line, lineSize, commaIx + 1);
                    argSize = bsMatchIdent(line, lineSize, nextIx);
                    if (argSize == 0) {
                        break;
                    }
                    ix = nextIx;
                }
                size_t dotsIx = bsSkipSpace(line, lineSize, ix);
                if (lineSize - dotsIx >= 3 && memcmp(line + dotsIx, "...", 3) == 0) {
                    lastArgArray = true;
                    ix = dotsIx + 3;
                }
                ix = bsSkipSpace(line, lineSize, ix);
                if (ix >= lineSize || line[ix] != ')') {
                    matched = false;
                } else {
                    ix = bsSkipSpace(line, lineSize, ix + 1);
                    if (ix >= lineSize || line[ix] != ':' || !bsMatchComment(line, lineSize, ix + 1)) {
                        matched = false;
                    }
                }
            }

            if (matched) {
                if (parser->functionDef != NULL) {
                    bsScriptParserError(parser, "Nested function definition", line, lineSize, 1, errorLineNumber);
                } else {
                    BSFunctionDef *def = bsAlloc(sizeof(BSFunctionDef));
                    memset(def, 0, sizeof(BSFunctionDef));
                    def->name = bsRetain(name);
                    def->argNames = argNames;
                    def->argCount = argCount;
                    def->lastArgArray = lastArgArray;
                    def->async = async;
                    def->lineNumber = lineNumber;
                    def->lineCount = lineCount;
                    argNames = NULL;
                    argCount = 0;

                    BSStatement *statement = bsStatementAdd(statements, BS_STMT_FUNCTION, lineNumber, lineCount);
                    statement->u.function.def = def;
                    bsScriptAddFunction(parser->script, def, functionCapacity);
                    parser->functionDef = def;
                    parser->functionLabelDefDepth = parser->labelDefCount;
                }
                bsRelease(name);
                for (size_t ixFree = 0; ixFree < argCount; ixFree++) {
                    bsRelease(argNames[ixFree]);
                }
                free(argNames);
                return;
            }
            bsRelease(name);
            for (size_t ixFree = 0; ixFree < argCount; ixFree++) {
                bsRelease(argNames[ixFree]);
            }
            free(argNames);
        }

        /* A function definition end */
        if (keywordSize == 11 && memcmp(keyword, "endfunction", 11) == 0 &&
            bsMatchComment(line, lineSize, keywordEnd)) {
            if (parser->functionDef == NULL) {
                bsScriptParserError(parser, "No matching function definition", line, lineSize, 1, errorLineNumber);
                return;
            }
            if (parser->labelDefCount > parser->functionLabelDefDepth) {
                BSLabelDef *def = &parser->labelDefs[parser->labelDefCount - 1];
                static const char *endNames[] = {"Missing endif statement", "Missing endwhile statement",
                                                 "Missing endfor statement"};
                bsScriptParserError(parser, endNames[def->type], bsStringData(def->line),
                                    bsStringSize(def->line), 1, def->lineNumber);
                return;
            }
            parser->functionDef->statements = parser->functionStatements.statements;
            parser->functionDef->statementCount = parser->functionStatements.count;
            memset(&parser->functionStatements, 0, sizeof(parser->functionStatements));
            parser->functionDef = NULL;
            parser->functionLabelDefDepth = 0;
            return;
        }

        /* An if-then */
        if (keywordSize == 2 && memcmp(keyword, "if", 2) == 0) {
            size_t exprBegin;
            size_t exprEnd;
            if (bsMatchBlockExpr(line, lineSize, keywordEnd, &exprBegin, &exprEnd)) {
                BSExpr *expr = bsParseLineExpression(parser, line + exprBegin, exprEnd - exprBegin, line,
                                                     lineSize, bsColumnOffset(line, exprBegin), errorLineNumber);
                if (expr == NULL) {
                    return;
                }
                BSLabelDef def;
                memset(&def, 0, sizeof(def));
                def.type = BS_LABEL_IF;
                def.done = bsGeneratedLabel("__barescriptDone", parser->labelIndex);
                def.loop = bsNull();
                def.continueLabel = bsNull();
                def.indexName = bsNull();
                def.valuesName = bsNull();
                def.lengthName = bsNull();
                def.valueName = bsNull();
                def.line = bsRetain(lineValue);
                def.lineNumber = errorLineNumber;
                BSValue labelIf = bsGeneratedLabel("__barescriptIf", parser->labelIndex);
                def.jump = bsAddJump(statements, labelIf, bsExprUnary(BS_UNARY_NOT, expr), lineNumber, lineCount);
                bsRelease(labelIf);
                bsLabelDefPush(parser, &def);
                parser->labelIndex++;
                return;
            }
        }

        /* An else-if-then */
        if (keywordSize == 4 && memcmp(keyword, "elif", 4) == 0) {
            size_t exprBegin;
            size_t exprEnd;
            if (bsMatchBlockExpr(line, lineSize, keywordEnd, &exprBegin, &exprEnd)) {
                size_t depth = bsLabelDefDepth(parser);
                BSLabelDef *def = (parser->labelDefCount > depth &&
                                   parser->labelDefs[parser->labelDefCount - 1].type == BS_LABEL_IF) ?
                    &parser->labelDefs[parser->labelDefCount - 1] : NULL;
                if (def == NULL) {
                    bsScriptParserError(parser, "No matching if statement", line, lineSize, 1, errorLineNumber);
                    return;
                }
                if (def->hasElse) {
                    bsScriptParserError(parser, "Elif statement following else statement", line, lineSize, 1,
                                        errorLineNumber);
                    return;
                }
                BSExpr *expr = bsParseLineExpression(parser, line + exprBegin, exprEnd - exprBegin, line,
                                                     lineSize, bsColumnOffset(line, exprBegin), errorLineNumber);
                if (expr == NULL) {
                    return;
                }
                BSValue prevLabel = bsRetain(def->jump->u.jump.label);
                BSValue labelIf = bsGeneratedLabel("__barescriptIf", parser->labelIndex);
                parser->labelIndex++;
                bsAddJump(statements, def->done, NULL, lineNumber, lineCount);
                bsAddLabel(statements, prevLabel, lineNumber, lineCount);
                def->jump = bsAddJump(statements, labelIf, bsExprUnary(BS_UNARY_NOT, expr), lineNumber, lineCount);
                bsRelease(labelIf);
                bsRelease(prevLabel);
                return;
            }
        }

        /* An else-then */
        if (keywordSize == 4 && memcmp(keyword, "else", 4) == 0) {
            size_t colonIx = bsSkipSpace(line, lineSize, keywordEnd);
            if (colonIx < lineSize && line[colonIx] == ':' && bsMatchComment(line, lineSize, colonIx + 1)) {
                size_t depth = bsLabelDefDepth(parser);
                BSLabelDef *def = (parser->labelDefCount > depth &&
                                   parser->labelDefs[parser->labelDefCount - 1].type == BS_LABEL_IF) ?
                    &parser->labelDefs[parser->labelDefCount - 1] : NULL;
                if (def == NULL) {
                    bsScriptParserError(parser, "No matching if statement", line, lineSize, 1, errorLineNumber);
                    return;
                }
                if (def->hasElse) {
                    bsScriptParserError(parser, "Multiple else statements", line, lineSize, 1, errorLineNumber);
                    return;
                }
                def->hasElse = true;
                bsAddJump(statements, def->done, NULL, lineNumber, lineCount);
                bsAddLabel(statements, def->jump->u.jump.label, lineNumber, lineCount);
                return;
            }
        }

        /* An if-then end */
        if (keywordSize == 5 && memcmp(keyword, "endif", 5) == 0 && bsMatchComment(line, lineSize, keywordEnd)) {
            size_t depth = bsLabelDefDepth(parser);
            if (parser->labelDefCount <= depth || parser->labelDefs[parser->labelDefCount - 1].type != BS_LABEL_IF) {
                bsScriptParserError(parser, "No matching if statement", line, lineSize, 1, errorLineNumber);
                return;
            }
            BSLabelDef def = parser->labelDefs[--parser->labelDefCount];
            if (!def.hasElse) {
                bsAssign(&def.jump->u.jump.label, bsRetain(def.done));
            }
            bsAddLabel(statements, def.done, lineNumber, lineCount);
            bsLabelDefFree(&def);
            return;
        }

        /* A while-do */
        if (keywordSize == 5 && memcmp(keyword, "while", 5) == 0) {
            size_t exprBegin;
            size_t exprEnd;
            if (bsMatchBlockExpr(line, lineSize, keywordEnd, &exprBegin, &exprEnd)) {
                BSExpr *expr = bsParseLineExpression(parser, line + exprBegin, exprEnd - exprBegin, line,
                                                     lineSize, bsColumnOffset(line, exprBegin), errorLineNumber);
                if (expr == NULL) {
                    return;
                }
                BSLabelDef def;
                memset(&def, 0, sizeof(def));
                def.type = BS_LABEL_WHILE;
                def.loop = bsGeneratedLabel("__barescriptLoop", parser->labelIndex);
                def.continueLabel = bsGeneratedLabel("__barescriptContinue", parser->labelIndex);
                def.done = bsGeneratedLabel("__barescriptDone", parser->labelIndex);
                def.indexName = bsNull();
                def.valuesName = bsNull();
                def.lengthName = bsNull();
                def.valueName = bsNull();
                def.expr = expr;
                def.line = bsRetain(lineValue);
                def.lineNumber = errorLineNumber;
                parser->labelIndex++;
                bsAddJump(statements, def.done, bsExprUnary(BS_UNARY_NOT, bsExprCopy(expr)), lineNumber, lineCount);
                bsAddLabel(statements, def.loop, lineNumber, lineCount);
                bsLabelDefPush(parser, &def);
                return;
            }
        }

        /* A while-do end */
        if (keywordSize == 8 && memcmp(keyword, "endwhile", 8) == 0 && bsMatchComment(line, lineSize, keywordEnd)) {
            size_t depth = bsLabelDefDepth(parser);
            if (parser->labelDefCount <= depth ||
                parser->labelDefs[parser->labelDefCount - 1].type != BS_LABEL_WHILE) {
                bsScriptParserError(parser, "No matching while statement", line, lineSize, 1, errorLineNumber);
                return;
            }
            BSLabelDef def = parser->labelDefs[--parser->labelDefCount];
            if (def.hasContinue) {
                bsAddLabel(statements, def.continueLabel, lineNumber, lineCount);
            }
            bsAddJump(statements, def.loop, def.expr, lineNumber, lineCount);
            def.expr = NULL;
            bsAddLabel(statements, def.done, lineNumber, lineCount);
            bsLabelDefFree(&def);
            return;
        }

        /* A for-each */
        if (keywordSize == 3 && memcmp(keyword, "for", 3) == 0) {
            size_t ix = bsSkipSpace(line, lineSize, keywordEnd);
            size_t valueSize = (ix != keywordEnd) ? bsMatchIdent(line, lineSize, ix) : 0;
            if (valueSize != 0) {
                BSValue valueName = bsStringNewSize(line + ix, valueSize);
                BSValue indexName = bsNull();
                ix += valueSize;
                size_t commaIx = bsSkipSpace(line, lineSize, ix);
                if (commaIx < lineSize && line[commaIx] == ',') {
                    size_t indexIx = bsSkipSpace(line, lineSize, commaIx + 1);
                    size_t indexSize = bsMatchIdent(line, lineSize, indexIx);
                    if (indexSize != 0) {
                        indexName = bsStringNewSize(line + indexIx, indexSize);
                        ix = indexIx + indexSize;
                    }
                }
                size_t inIx = bsSkipSpace(line, lineSize, ix);
                size_t inEnd;
                size_t exprBegin = 0;
                size_t exprEnd = 0;
                bool matched = inIx != ix && bsMatchKeyword(line, lineSize, inIx, "in", &inEnd);
                if (matched) {
                    matched = bsMatchBlockExpr(line, lineSize, inEnd, &exprBegin, &exprEnd);
                }
                if (matched) {
                    BSLabelDef def;
                    memset(&def, 0, sizeof(def));
                    def.type = BS_LABEL_FOR;
                    def.loop = bsGeneratedLabel("__barescriptLoop", parser->labelIndex);
                    def.continueLabel = bsGeneratedLabel("__barescriptContinue", parser->labelIndex);
                    def.done = bsGeneratedLabel("__barescriptDone", parser->labelIndex);
                    def.indexName = indexName.type == BS_STRING ? bsRetain(indexName) :
                        bsGeneratedLabel("__barescriptIndex", parser->labelIndex);
                    def.valuesName = bsGeneratedLabel("__barescriptValues", parser->labelIndex);
                    def.lengthName = bsGeneratedLabel("__barescriptLength", parser->labelIndex);
                    def.valueName = bsRetain(valueName);
                    def.line = bsRetain(lineValue);
                    def.lineNumber = errorLineNumber;
                    bsLabelDefPush(parser, &def);
                    parser->labelIndex++;
                    bsRelease(valueName);
                    bsRelease(indexName);

                    BSExpr *expr = bsParseLineExpression(parser, line + exprBegin, exprEnd - exprBegin, line,
                                                         lineSize, bsColumnOffset(line, exprBegin),
                                                         errorLineNumber);
                    if (expr == NULL) {
                        return;
                    }
                    BSLabelDef *pushed = &parser->labelDefs[parser->labelDefCount - 1];
                    BSExpr *lengthArgs[1];
                    BSExpr *getArgs[2];
                    bsAddAssign(statements, pushed->valuesName, expr, lineNumber, lineCount);
                    lengthArgs[0] = bsExprVariable(pushed->valuesName);
                    bsAddAssign(statements, pushed->lengthName, bsExprCall("arrayLength", lengthArgs, 1),
                                lineNumber, lineCount);
                    bsAddJump(statements, pushed->done,
                              bsExprUnary(BS_UNARY_NOT, bsExprVariable(pushed->lengthName)), lineNumber, lineCount);
                    bsAddAssign(statements, pushed->indexName, bsExprNumber(0), lineNumber, lineCount);
                    bsAddLabel(statements, pushed->loop, lineNumber, lineCount);
                    getArgs[0] = bsExprVariable(pushed->valuesName);
                    getArgs[1] = bsExprVariable(pushed->indexName);
                    bsAddAssign(statements, pushed->valueName, bsExprCall("arrayGet", getArgs, 2),
                                lineNumber, lineCount);
                    return;
                }
                bsRelease(valueName);
                bsRelease(indexName);
            }
        }

        /* A for-each end */
        if (keywordSize == 6 && memcmp(keyword, "endfor", 6) == 0 && bsMatchComment(line, lineSize, keywordEnd)) {
            size_t depth = bsLabelDefDepth(parser);
            if (parser->labelDefCount <= depth || parser->labelDefs[parser->labelDefCount - 1].type != BS_LABEL_FOR) {
                bsScriptParserError(parser, "No matching for statement", line, lineSize, 1, errorLineNumber);
                return;
            }
            BSLabelDef def = parser->labelDefs[--parser->labelDefCount];
            if (def.hasContinue) {
                bsAddLabel(statements, def.continueLabel, lineNumber, lineCount);
            }
            bsAddAssign(statements, def.indexName,
                        bsExprBinary(BS_BINARY_ADD, bsExprVariable(def.indexName), bsExprNumber(1)),
                        lineNumber, lineCount);
            bsAddJump(statements, def.loop,
                      bsExprBinary(BS_BINARY_LT, bsExprVariable(def.indexName), bsExprVariable(def.lengthName)),
                      lineNumber, lineCount);
            bsAddLabel(statements, def.done, lineNumber, lineCount);
            bsLabelDefFree(&def);
            return;
        }

        /* A break or continue statement */
        bool isBreak = (keywordSize == 5 && memcmp(keyword, "break", 5) == 0);
        bool isContinue = (keywordSize == 8 && memcmp(keyword, "continue", 8) == 0);
        if ((isBreak || isContinue) && bsMatchComment(line, lineSize, keywordEnd)) {
            size_t depth = bsLabelDefDepth(parser);
            size_t ixLabelDef = parser->labelDefCount;
            while (ixLabelDef > 0 && parser->labelDefs[ixLabelDef - 1].type == BS_LABEL_IF) {
                ixLabelDef--;
            }
            if (ixLabelDef <= depth) {
                bsScriptParserError(parser, isBreak ? "Break statement outside of loop" :
                                    "Continue statement outside of loop", line, lineSize, 1, errorLineNumber);
                return;
            }
            BSLabelDef *def = &parser->labelDefs[ixLabelDef - 1];
            if (isBreak) {
                bsAddJump(statements, def->done, NULL, lineNumber, lineCount);
            } else {
                def->hasContinue = true;
                bsAddJump(statements, def->continueLabel, NULL, lineNumber, lineCount);
            }
            return;
        }

        /* A jump or jumpif statement */
        if ((keywordSize == 4 && memcmp(keyword, "jump", 4) == 0) ||
            (keywordSize == 6 && memcmp(keyword, "jumpif", 6) == 0)) {
            bool isJumpIf = (keywordSize == 6);
            size_t exprBegin = 0;
            size_t exprEnd = 0;
            size_t labelBegin = 0;
            size_t labelSize = 0;
            bool matched = false;
            if (!isJumpIf) {
                size_t nameIx = bsSkipSpace(line, lineSize, keywordEnd);
                labelSize = (nameIx != keywordEnd) ? bsMatchIdent(line, lineSize, nameIx) : 0;
                labelBegin = nameIx;
                matched = labelSize != 0 && bsMatchComment(line, lineSize, nameIx + labelSize);
            } else {
                size_t openIx = bsSkipSpace(line, lineSize, keywordEnd);
                if (openIx < lineSize && line[openIx] == '(') {
                    /* The test expression's ".+" is greedy - find the last valid close parenthesis */
                    for (size_t ix = lineSize; ix > openIx + 1 && !matched; ix--) {
                        if (line[ix - 1] != ')') {
                            continue;
                        }
                        size_t nameIx = bsSkipSpace(line, lineSize, ix);
                        size_t nameSize = (nameIx != ix) ? bsMatchIdent(line, lineSize, nameIx) : 0;
                        if (nameSize != 0 && bsMatchComment(line, lineSize, nameIx + nameSize)) {
                            exprBegin = openIx + 1;
                            exprEnd = ix - 1;
                            labelBegin = nameIx;
                            labelSize = nameSize;
                            matched = true;
                        }
                    }
                }
            }
            if (matched) {
                BSExpr *expr = NULL;
                if (isJumpIf) {
                    expr = bsParseLineExpression(parser, line + exprBegin, exprEnd - exprBegin, line, lineSize,
                                                 bsColumnOffset(line, exprBegin), errorLineNumber);
                    if (expr == NULL) {
                        return;
                    }
                }
                BSValue label = bsStringNewSize(line + labelBegin, labelSize);
                bsAddJump(statements, label, expr, lineNumber, lineCount);
                bsRelease(label);
                return;
            }
        }

        /* A return statement */
        if (keywordSize == 6 && memcmp(keyword, "return", 6) == 0) {
            size_t exprBegin = bsSkipSpace(line, lineSize, keywordEnd);
            bool hasExpr = (exprBegin != keywordEnd && exprBegin < lineSize && line[exprBegin] != '#');
            if (hasExpr || bsMatchComment(line, lineSize, keywordEnd)) {
                BSExpr *expr = NULL;
                if (hasExpr) {
                    expr = bsParseLineExpression(parser, line + exprBegin, lineSize - exprBegin, line, lineSize,
                                                 bsColumnOffset(line, exprBegin), errorLineNumber);
                    if (expr == NULL) {
                        return;
                    }
                }
                BSStatement *statement = bsStatementAdd(statements, BS_STMT_RETURN, lineNumber, lineCount);
                statement->u.ret.expr = expr;
                return;
            }
        }

        /* An include statement */
        if (keywordSize == 7 && memcmp(keyword, "include", 7) == 0) {
            size_t urlIx = bsSkipSpace(line, lineSize, keywordEnd);
            bool system = false;
            bool matched = false;
            BSValue url = bsNull();
            if (urlIx != keywordEnd && urlIx < lineSize && (line[urlIx] == '\'' || line[urlIx] == '<')) {
                system = (line[urlIx] == '<');
                char close = system ? '>' : '\'';
                size_t begin = urlIx + 1;
                size_t end = begin;
                while (end < lineSize && line[end] != close) {
                    if (!system && line[end] == '\\' && end + 1 < lineSize && line[end + 1] == '\'') {
                        end += 2;
                    } else {
                        end++;
                    }
                }
                if (end < lineSize && bsMatchComment(line, lineSize, end + 1)) {
                    url = system ? bsStringNewSize(line + begin, end - begin) :
                        bsStringEscape(line + begin, end - begin);
                    matched = true;
                }
            }
            if (matched) {
                BSStatement *statement = (statements->count != 0 &&
                                          statements->statements[statements->count - 1]->type == BS_STMT_INCLUDE) ?
                    statements->statements[statements->count - 1] : NULL;
                if (statement == NULL) {
                    statement = bsStatementAdd(statements, BS_STMT_INCLUDE, lineNumber, lineCount);
                } else {
                    statement->lineCount = ixLinePart - statement->lineNumber + 2;
                }
                statement->u.include.includes = bsRealloc(statement->u.include.includes,
                                                          (statement->u.include.count + 1) * sizeof(BSInclude));
                statement->u.include.includes[statement->u.include.count].url = url;
                statement->u.include.includes[statement->u.include.count].system = system;
                statement->u.include.count++;
                return;
            }
        }
    }

    /* The catch-all - an assignment, a label definition, or an expression statement */
    if (keywordSize != 0) {
        /* An assignment */
        size_t equalIx = bsSkipSpace(line, lineSize, keywordEnd);
        if (equalIx < lineSize && line[equalIx] == '=') {
            size_t exprBegin = bsSkipSpace(line, lineSize, equalIx + 1);
            if (exprBegin == lineSize && exprBegin > equalIx + 1) {
                exprBegin = lineSize - 1;
            }
            if (exprBegin < lineSize) {
                BSExpr *expr = bsParseLineExpression(parser, line + exprBegin, lineSize - exprBegin, line,
                                                     lineSize, bsColumnOffset(line, exprBegin), errorLineNumber);
                if (expr == NULL) {
                    return;
                }
                BSValue name = bsStringNewSize(line + keywordBegin, keywordSize);
                bsAddAssign(statements, name, expr, lineNumber, lineCount);
                bsRelease(name);
                return;
            }
        }

        /* A label definition */
        size_t colonIx = bsSkipSpace(line, lineSize, keywordEnd);
        if (colonIx < lineSize && line[colonIx] == ':' && bsMatchComment(line, lineSize, colonIx + 1)) {
            BSValue name = bsStringNewSize(line + keywordBegin, keywordSize);
            bsAddLabel(statements, name, lineNumber, lineCount);
            bsRelease(name);
            return;
        }
    }

    /* An expression statement */
    BSExpr *expr = bsParseLineExpression(parser, line, lineSize, line, lineSize, 0, errorLineNumber);
    if (expr == NULL) {
        return;
    }
    BSStatement *statement = bsStatementAdd(statements, BS_STMT_EXPR, lineNumber, lineCount);
    statement->u.expr.expr = expr;
}

BSScript *bsParseScript(const char *text, size_t size, int startLineNumber, const char *scriptName,
                        BSParserError *error)
{
    BSScript *script = bsAlloc(sizeof(BSScript));
    memset(script, 0, sizeof(BSScript));
    script->refcount = 1;
    script->scriptName = scriptName != NULL ? bsStringNew(scriptName) : bsNull();
    script->scriptLines = bsArrayNew();

    /* Split the script text into lines */
    size_t lineBegin = 0;
    for (size_t ix = 0; ix <= size; ix++) {
        if (ix == size || text[ix] == '\n') {
            size_t lineEnd = ix;
            if (lineEnd > lineBegin && text[lineEnd - 1] == '\r') {
                lineEnd--;
            }
            bsArrayPush(script->scriptLines, bsStringNewSize(text + lineBegin, lineEnd - lineBegin));
            lineBegin = ix + 1;
        }
    }

    BSScriptParser parser;
    memset(&parser, 0, sizeof(parser));
    parser.script = script;
    parser.statements = &parser.top;
    parser.startLineNumber = startLineNumber;
    parser.scriptName = scriptName;
    parser.error = error;

    size_t functionCapacity = 0;
    size_t lineCount = bsArrayCount(script->scriptLines);

    /* The pending line continuation parts */
    BSStringBuilder continuation;
    bsSBInit(&continuation);
    bool isContinued = false;
    size_t ixLine = 0;

    for (size_t ixLinePart = 0; ixLinePart < lineCount && !parser.failed; ixLinePart++) {
        BSValue linePartValue = bsArrayGet(script->scriptLines, ixLinePart);
        const char *linePart = bsStringData(linePartValue);
        size_t linePartSize = bsStringSize(linePartValue);

        /* Skip a comment or empty line */
        size_t trimBegin = bsSkipSpace(linePart, linePartSize, 0);
        if (trimBegin == linePartSize || linePart[trimBegin] == '#') {
            continue;
        }

        if (!isContinued) {
            ixLine = ixLinePart;
        }

        /* A line continuation - a trailing backslash followed by optional whitespace */
        size_t noContinuationSize = linePartSize;
        while (noContinuationSize > 0 && bsIsSpace(linePart[noContinuationSize - 1])) {
            noContinuationSize--;
        }
        bool continued = (noContinuationSize > 0 && linePart[noContinuationSize - 1] == '\\');
        if (continued) {
            noContinuationSize--;
        } else {
            noContinuationSize = linePartSize;
        }

        if (continued) {
            size_t partBegin = 0;
            size_t partEnd = noContinuationSize;
            if (isContinued) {
                partBegin = bsSkipSpace(linePart, partEnd, 0);
            }
            while (partEnd > partBegin && bsIsSpace(linePart[partEnd - 1])) {
                partEnd--;
            }
            if (isContinued) {
                bsSBAppendChar(&continuation, ' ');
            }
            bsSBAppend(&continuation, linePart + partBegin, partEnd - partBegin);
            isContinued = true;
            continue;
        }
        if (isContinued) {
            size_t partBegin = bsSkipSpace(linePart, noContinuationSize, 0);
            size_t partEnd = noContinuationSize;
            while (partEnd > partBegin && bsIsSpace(linePart[partEnd - 1])) {
                partEnd--;
            }
            bsSBAppendChar(&continuation, ' ');
            bsSBAppend(&continuation, linePart + partBegin, partEnd - partBegin);
        }

        /* The logical line - the joined continuation parts, or the line itself */
        BSValue lineValue;
        if (isContinued) {
            lineValue = bsSBToValue(&continuation);
            bsSBInit(&continuation);
            isContinued = false;
        } else {
            lineValue = bsRetain(linePartValue);
        }
        const char *line = bsStringData(lineValue);
        size_t lineSize = bsStringSize(lineValue);

        int lineNumber = (int) ixLine + 1;
        int statementLineCount = (ixLine != ixLinePart ? (int) (ixLinePart - ixLine) + 1 : 0);
        int errorLineNumber = startLineNumber + (int) ixLine;
        bsParseStatement(&parser, line, lineSize, lineValue, lineNumber, statementLineCount,
                         errorLineNumber, (int) ixLinePart, &functionCapacity);
        bsRelease(lineValue);
    }
    bsSBFree(&continuation);

    /* A dangling structured statement */
    if (!parser.failed && parser.labelDefCount > 0) {
        BSLabelDef *def = &parser.labelDefs[parser.labelDefCount - 1];
        static const char *endNames[] = {"Missing endif statement", "Missing endwhile statement",
                                         "Missing endfor statement"};
        bsScriptParserError(&parser, endNames[def->type], bsStringData(def->line),
                            bsStringSize(def->line), 1, def->lineNumber);
    }

    if (parser.failed) {
        for (size_t ix = 0; ix < parser.labelDefCount; ix++) {
            bsLabelDefFree(&parser.labelDefs[ix]);
        }
        free(parser.labelDefs);
        bsStatementsFree(parser.top.statements, parser.top.count);
        bsStatementsFree(parser.functionStatements.statements, parser.functionStatements.count);
        if (parser.functionDef != NULL) {
            parser.functionDef->statements = NULL;
            parser.functionDef->statementCount = 0;
        }
        script->statements = NULL;
        script->statementCount = 0;
        bsScriptRelease(script);
        return NULL;
    }

    free(parser.labelDefs);
    script->statements = parser.top.statements;
    script->statementCount = parser.top.count;
    bsResolveJumps(script->statements, script->statementCount);
    for (size_t ix = 0; ix < script->functionCount; ix++) {
        BSFunctionDef *def = script->functions[ix];
        def->script = script;
        bsResolveJumps(def->statements, def->statementCount);
        bsResolveSlots(def);
    }
    return script;
}
