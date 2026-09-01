/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The BareScript runtime
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "barescript/library.h"
#include "barescript/options.h"
#include "barescript/runtime.h"

#include "internal.h"


/* The maximum expression evaluation recursion depth */
#define BS_DEPTH_MAX 500

/* The largest datetime JavaScript's Date represents, in milliseconds */
#define BS_DATETIME_MAX 8640000000000000.0


/*
 * The script execution options
 */


BSOptions *bsOptionsNew(void)
{
    BSOptions *options = bsAlloc(sizeof(BSOptions));
    memset(options, 0, sizeof(BSOptions));
    options->globals = bsObjectNew();
    options->error = bsNull();
    options->argsError = bsNull();
    options->maxStatements = BS_MAX_STATEMENTS_DEFAULT;
    options->depthMax = BS_DEPTH_MAX;
    return options;
}


void bsOptionsFree(BSOptions *options)
{
    if (options == NULL) {
        return;
    }
    if (options->urlDataFree != NULL) {
        options->urlDataFree(options->urlData);
    }
    bsRelease(options->globals);
    bsRelease(options->error);
    bsRelease(options->argsError);
    free(options);
}


void bsErrorSet(BSOptions *options, const char *format, ...)
{
    if (options->error.type == BS_STRING) {
        return;
    }
    va_list args;
    va_start(args, format);
    options->error = bsStringNewVFormat(format, args);
    va_end(args);
}


void bsErrorSetStatement(BSOptions *options, const BSScript *script, const BSStatement *statement,
                         const char *format, ...)
{
    if (options->error.type == BS_STRING) {
        return;
    }
    va_list args;
    va_start(args, format);
    BSValue message = bsStringNewVFormat(format, args);
    va_end(args);

    if (script != NULL && statement != NULL) {
        const char *scriptName = script->scriptName.type == BS_STRING ? bsStringData(script->scriptName) : "";
        options->error = bsStringNewFormat("%s:%d: %s", scriptName, statement->lineNumber,
                                           bsStringData(message));
        bsRelease(message);
    } else {
        options->error = message;
    }
}


const char *bsErrorGet(const BSOptions *options)
{
    return options->error.type == BS_STRING ? bsStringData(options->error) : NULL;
}


void bsErrorClear(BSOptions *options)
{
    bsAssign(&options->error, bsNull());
}


void bsLog(BSOptions *options, const char *format, ...)
{
    if (options->logFn == NULL) {
        return;
    }
    va_list args;
    va_start(args, format);
    BSValue text = bsStringNewVFormat(format, args);
    va_end(args);
    options->logFn(bsStringData(text), options->logData);
    bsRelease(text);
}


void bsScopeInit(BSScope *scope)
{
    scope->slots = NULL;
    scope->slotCount = 0;
    scope->object = bsNull();
}


/*
 * The system include library
 */


static BSValue bsSystemIncludes = {BS_NULL, {0}};
static BSValue bsSystemIncludePaths = {BS_NULL, {0}};


void bsSystemIncludeRegister(const char *name, const char *text)
{
    if (bsSystemIncludes.type != BS_OBJECT) {
        bsSystemIncludes = bsObjectNew();
    }
    bsObjectSet(bsSystemIncludes, name, bsStringNew(text));
}


void bsSystemIncludePath(const char *directory)
{
    if (bsSystemIncludePaths.type != BS_ARRAY) {
        bsSystemIncludePaths = bsArrayNew();
    }
    bsArrayPush(bsSystemIncludePaths, bsStringNew(directory));
}


const char *bsSystemIncludeGet(const char *name)
{
    BSValue text = bsObjectGet(bsSystemIncludes, name);
    if (text.type == BS_STRING) {
        return bsStringData(text);
    }

    /* Search the registered directories, caching what is found */
    for (size_t ix = 0; ix < bsArrayCount(bsSystemIncludePaths); ix++) {
        BSValue directory = bsArrayGet(bsSystemIncludePaths, ix);
        BSValue path = bsStringNewFormat("%s/%s", bsStringData(directory), name);
        BSFetchRequest request;
        memset(&request, 0, sizeof(request));
        request.url = bsStringData(path);
        request.headers = bsNull();
        size_t size = 0;
        char *fileText = bsFetchReadOnly(&request, &size, NULL);
        bsRelease(path);
        if (fileText != NULL) {
            if (bsSystemIncludes.type != BS_OBJECT) {
                bsSystemIncludes = bsObjectNew();
            }
            bsObjectSet(bsSystemIncludes, name, bsStringNewSize(fileText, size));
            free(fileText);
            return bsStringData(bsObjectGet(bsSystemIncludes, name));
        }
    }
    return NULL;
}


void bsSystemIncludeClear(void)
{
    bsRelease(bsSystemIncludes);
    bsSystemIncludes = bsNull();
    bsRelease(bsSystemIncludePaths);
    bsSystemIncludePaths = bsNull();
}


/*
 * Model conversion
 */


BSValue bsExprToModel(const BSExpr *expr)
{
    BSValue model = bsObjectNew();
    switch (expr->type) {
    case BS_EXPR_NUMBER:
        bsObjectSet(model, "number", bsNumber(expr->u.number));
        break;

    case BS_EXPR_STRING:
        bsObjectSet(model, "string", bsRetain(expr->u.string));
        break;

    case BS_EXPR_VARIABLE:
        bsObjectSet(model, "variable", bsRetain(expr->u.variable.name));
        break;

    case BS_EXPR_FUNCTION: {
        BSValue function = bsObjectNew();
        bsObjectSet(function, "name", bsRetain(expr->u.function.name));
        BSValue args = bsArrayNewCapacity(expr->u.function.argCount);
        for (size_t ix = 0; ix < expr->u.function.argCount; ix++) {
            bsArrayPush(args, bsExprToModel(expr->u.function.args[ix]));
        }
        bsObjectSet(function, "args", args);
        bsObjectSet(model, "function", function);
        break;
    }

    case BS_EXPR_BINARY: {
        BSValue binary = bsObjectNew();
        bsObjectSet(binary, "op", bsStringNew(bsBinaryOpText[expr->u.binary.op]));
        bsObjectSet(binary, "left", bsExprToModel(expr->u.binary.left));
        bsObjectSet(binary, "right", bsExprToModel(expr->u.binary.right));
        bsObjectSet(model, "binary", binary);
        break;
    }

    case BS_EXPR_UNARY: {
        BSValue unary = bsObjectNew();
        bsObjectSet(unary, "op", bsStringNew(bsUnaryOpText[expr->u.unary.op]));
        bsObjectSet(unary, "expr", bsExprToModel(expr->u.unary.expr));
        bsObjectSet(model, "unary", unary);
        break;
    }

    default:
        bsObjectSet(model, "group", bsExprToModel(expr->u.group));
        break;
    }
    return model;
}


static BSValue bsStatementToModel(const BSStatement *statement)
{
    BSValue model = bsObjectNew();
    BSValue value = bsObjectNew();
    if (statement->lineNumber != 0) {
        bsObjectSet(value, "lineNumber", bsNumber(statement->lineNumber));
    }
    if (statement->lineCount != 0) {
        bsObjectSet(value, "lineCount", bsNumber(statement->lineCount));
    }

    switch (statement->type) {
    case BS_STMT_EXPR:
        if (statement->u.expr.name.type == BS_STRING) {
            bsObjectSet(value, "name", bsRetain(statement->u.expr.name));
        }
        bsObjectSet(value, "expr", bsExprToModel(statement->u.expr.expr));
        bsObjectSet(model, "expr", value);
        break;

    case BS_STMT_JUMP:
        bsObjectSet(value, "label", bsRetain(statement->u.jump.label));
        if (statement->u.jump.expr != NULL) {
            bsObjectSet(value, "expr", bsExprToModel(statement->u.jump.expr));
        }
        bsObjectSet(model, "jump", value);
        break;

    case BS_STMT_RETURN:
        if (statement->u.ret.expr != NULL) {
            bsObjectSet(value, "expr", bsExprToModel(statement->u.ret.expr));
        }
        bsObjectSet(model, "return", value);
        break;

    case BS_STMT_LABEL:
        bsObjectSet(value, "name", bsRetain(statement->u.label.name));
        bsObjectSet(model, "label", value);
        break;

    case BS_STMT_FUNCTION: {
        const BSFunctionDef *def = statement->u.function.def;
        if (def->async) {
            bsObjectSet(value, "async", bsBoolean(true));
        }
        bsObjectSet(value, "name", bsRetain(def->name));
        if (def->argCount != 0) {
            BSValue args = bsArrayNewCapacity(def->argCount);
            for (size_t ix = 0; ix < def->argCount; ix++) {
                bsArrayPush(args, bsRetain(def->argNames[ix]));
            }
            bsObjectSet(value, "args", args);
        }
        if (def->lastArgArray) {
            bsObjectSet(value, "lastArgArray", bsBoolean(true));
        }
        BSValue statements = bsArrayNewCapacity(def->statementCount);
        for (size_t ix = 0; ix < def->statementCount; ix++) {
            bsArrayPush(statements, bsStatementToModel(def->statements[ix]));
        }
        bsObjectSet(value, "statements", statements);
        bsObjectSet(model, "function", value);
        break;
    }

    default: {
        BSValue includes = bsArrayNewCapacity(statement->u.include.count);
        for (size_t ix = 0; ix < statement->u.include.count; ix++) {
            BSValue include = bsObjectNew();
            bsObjectSet(include, "url", bsRetain(statement->u.include.includes[ix].url));
            if (statement->u.include.includes[ix].system) {
                bsObjectSet(include, "system", bsBoolean(true));
            }
            bsArrayPush(includes, include);
        }
        bsObjectSet(value, "includes", includes);
        bsObjectSet(model, "include", value);
        break;
    }
    }
    return model;
}


BSValue bsScriptToModel(const BSScript *script)
{
    BSValue model = bsObjectNew();
    BSValue statements = bsArrayNewCapacity(script->statementCount);
    for (size_t ix = 0; ix < script->statementCount; ix++) {
        bsArrayPush(statements, bsStatementToModel(script->statements[ix]));
    }
    bsObjectSet(model, "statements", statements);
    if (script->scriptName.type == BS_STRING) {
        bsObjectSet(model, "scriptName", bsRetain(script->scriptName));
    }
    if (bsArrayCount(script->scriptLines) != 0) {
        bsObjectSet(model, "scriptLines", bsRetain(script->scriptLines));
    }
    if (script->system) {
        bsObjectSet(model, "system", bsBoolean(true));
    }
    return model;
}


static BSExpr *bsExprModelNew(BSExprType type)
{
    BSExpr *expr = bsAlloc(sizeof(BSExpr));
    memset(expr, 0, sizeof(BSExpr));
    expr->type = type;
    return expr;
}


BSExpr *bsExprFromModel(BSValue model)
{
    if (model.type != BS_OBJECT) {
        return NULL;
    }

    BSValue number = bsObjectGet(model, "number");
    if (number.type == BS_NUMBER) {
        BSExpr *expr = bsExprModelNew(BS_EXPR_NUMBER);
        expr->u.number = number.u.number;
        return expr;
    }

    BSValue string = bsObjectGet(model, "string");
    if (string.type == BS_STRING) {
        BSExpr *expr = bsExprModelNew(BS_EXPR_STRING);
        expr->u.string = bsRetain(string);
        return expr;
    }

    BSValue variable = bsObjectGet(model, "variable");
    if (variable.type == BS_STRING) {
        BSExpr *expr = bsExprModelNew(BS_EXPR_VARIABLE);
        expr->u.variable.name = bsRetain(variable);
        expr->u.variable.slot = -1;
        const char *name = bsStringData(variable);
        if (strcmp(name, "null") == 0) {
            expr->u.variable.special = BS_SPECIAL_NULL;
        } else if (strcmp(name, "true") == 0) {
            expr->u.variable.special = BS_SPECIAL_TRUE;
        } else if (strcmp(name, "false") == 0) {
            expr->u.variable.special = BS_SPECIAL_FALSE;
        }
        return expr;
    }

    BSValue function = bsObjectGet(model, "function");
    if (function.type == BS_OBJECT) {
        BSValue name = bsObjectGet(function, "name");
        if (name.type != BS_STRING) {
            return NULL;
        }
        BSValue args = bsObjectGet(function, "args");
        BSExpr *expr = bsExprModelNew(BS_EXPR_FUNCTION);
        expr->u.function.name = bsRetain(name);
        expr->u.function.slot = -1;
        expr->u.function.isIf = (strcmp(bsStringData(name), "if") == 0);
        size_t argCount = bsArrayCount(args);
        if (argCount != 0) {
            expr->u.function.args = bsAlloc(argCount * sizeof(BSExpr *));
            for (size_t ix = 0; ix < argCount; ix++) {
                BSExpr *arg = bsExprFromModel(bsArrayGet(args, ix));
                if (arg == NULL) {
                    bsExprFree(expr);
                    return NULL;
                }
                expr->u.function.args[expr->u.function.argCount++] = arg;
            }
        }
        return expr;
    }

    BSValue binary = bsObjectGet(model, "binary");
    if (binary.type == BS_OBJECT) {
        BSValue op = bsObjectGet(binary, "op");
        if (op.type != BS_STRING) {
            return NULL;
        }
        int opIndex = -1;
        for (int ix = 0; ix < BS_BINARY_COUNT; ix++) {
            if (strcmp(bsBinaryOpText[ix], bsStringData(op)) == 0) {
                opIndex = ix;
                break;
            }
        }
        if (opIndex < 0) {
            return NULL;
        }
        BSExpr *left = bsExprFromModel(bsObjectGet(binary, "left"));
        BSExpr *right = bsExprFromModel(bsObjectGet(binary, "right"));
        if (left == NULL || right == NULL) {
            bsExprFree(left);
            bsExprFree(right);
            return NULL;
        }
        BSExpr *expr = bsExprModelNew(BS_EXPR_BINARY);
        expr->u.binary.op = (BSBinaryOp) opIndex;
        expr->u.binary.left = left;
        expr->u.binary.right = right;
        return expr;
    }

    BSValue unary = bsObjectGet(model, "unary");
    if (unary.type == BS_OBJECT) {
        BSValue op = bsObjectGet(unary, "op");
        if (op.type != BS_STRING) {
            return NULL;
        }
        int opIndex = -1;
        for (int ix = 0; ix < BS_UNARY_COUNT; ix++) {
            if (strcmp(bsUnaryOpText[ix], bsStringData(op)) == 0) {
                opIndex = ix;
                break;
            }
        }
        if (opIndex < 0) {
            return NULL;
        }
        BSExpr *operand = bsExprFromModel(bsObjectGet(unary, "expr"));
        if (operand == NULL) {
            return NULL;
        }
        BSExpr *expr = bsExprModelNew(BS_EXPR_UNARY);
        expr->u.unary.op = (BSUnaryOp) opIndex;
        expr->u.unary.expr = operand;
        return expr;
    }

    if (bsObjectHas(model, "group")) {
        BSExpr *inner = bsExprFromModel(bsObjectGet(model, "group"));
        if (inner == NULL) {
            return NULL;
        }
        BSExpr *expr = bsExprModelNew(BS_EXPR_GROUP);
        expr->u.group = inner;
        return expr;
    }

    return NULL;
}


/*
 * Expression evaluation
 */


static int32_t bsToInt32(double value)
{
    double modulo = fmod(trunc(value), 4294967296.0);
    if (modulo < 0) {
        modulo += 4294967296.0;
    }
    return (int32_t) (uint32_t) modulo;
}


static bool bsIsInteger(BSValue value)
{
    return value.type == BS_NUMBER && isfinite(value.u.number) && trunc(value.u.number) == value.u.number;
}


/* Normalize an arithmetic result - a non-finite result is an invalid operation */
static BSValue bsArithmetic(double result)
{
    return isfinite(result) ? bsNumber(result) : bsNull();
}


static BSValue bsEvalExpr(BSExpr *expr, BSOptions *options, BSScope *scope, bool builtins,
                          BSScript *script, BSStatement *statement);


/* Look up a variable's value - returns a borrowed reference */
static BSValue bsLookup(BSValue name, int slot, BSOptions *options, BSScope *scope)
{
    if (scope != NULL) {
        if (scope->slots != NULL) {
            if (slot >= 0) {
                BSValue value = scope->slots[slot];
                if (!BS_IS_UNSET(value)) {
                    return value;
                }
            }
        } else if (scope->object.type == BS_OBJECT && bsObjectHasString(scope->object, name)) {
            return bsObjectGetString(scope->object, name);
        }
    }
    return bsObjectGetString(options->globals, name);
}


static BSValue bsEvalFunction(BSExpr *expr, BSOptions *options, BSScope *scope, bool builtins,
                              BSScript *script, BSStatement *statement)
{
    /* The built-in "if" function evaluates only the selected branch */
    if (expr->u.function.isIf) {
        size_t argCount = expr->u.function.argCount;
        bool test = false;
        if (argCount >= 1) {
            BSValue value = bsEvalExpr(expr->u.function.args[0], options, scope, builtins, script, statement);
            test = bsValueBoolean(value);
            bsRelease(value);
        }
        BSExpr *branch = NULL;
        if (test) {
            branch = argCount >= 2 ? expr->u.function.args[1] : NULL;
        } else {
            branch = argCount >= 3 ? expr->u.function.args[2] : NULL;
        }
        return branch != NULL ? bsEvalExpr(branch, options, scope, builtins, script, statement) : bsNull();
    }

    /* Evaluate the function arguments */
    size_t argCount = expr->u.function.argCount;
    BSValue argsInline[8];
    BSValue *args = argCount <= 8 ? argsInline : bsAlloc(argCount * sizeof(BSValue));
    for (size_t ix = 0; ix < argCount; ix++) {
        args[ix] = bsEvalExpr(expr->u.function.args[ix], options, scope, builtins, script, statement);
    }

    /* Resolve the function value */
    BSValue function = bsLookup(expr->u.function.name, expr->u.function.slot, options, scope);
    if (function.type == BS_NULL && builtins) {
        function = bsLibraryExpressionFunction(expr->u.function.name);
    }

    BSValue result = bsNull();
    if (function.type == BS_FUNCTION) {
        result = bsFunctionCall(function, args, argCount, options);

        /* Log a library function argument error, which is not a runtime error */
        if (options->argsError.type == BS_STRING) {
            if (options->debug && options->logFn != NULL) {
                const char *scriptName = (script != NULL && script->scriptName.type == BS_STRING) ?
                    bsStringData(script->scriptName) : "";
                int lineNumber = statement != NULL ? statement->lineNumber : 0;
                bsLog(options, "%s:%d: BareScript: Function \"%s\" failed with error: %s", scriptName,
                      lineNumber, bsStringData(expr->u.function.name), bsStringData(options->argsError));
            }
            bsAssign(&options->argsError, bsNull());
        }
    } else if (function.type != BS_NULL) {
        /* A non-function value is not callable - the reference logs and evaluates to null */
        if (options->debug) {
            bsLog(options, "BareScript: Function \"%s\" failed with error: not a function",
                  bsStringData(expr->u.function.name));
        }
    } else {
        bsErrorSetStatement(options, script, statement, "Undefined function \"%s\"",
                            bsStringData(expr->u.function.name));
    }

    for (size_t ix = 0; ix < argCount; ix++) {
        bsRelease(args[ix]);
    }
    if (args != argsInline) {
        free(args);
    }
    return result;
}


static BSValue bsEvalBinary(BSExpr *expr, BSOptions *options, BSScope *scope, bool builtins,
                            BSScript *script, BSStatement *statement)
{
    BSBinaryOp op = expr->u.binary.op;
    BSValue left = bsEvalExpr(expr->u.binary.left, options, scope, builtins, script, statement);

    /* The short-circuiting logical operators */
    if (op == BS_BINARY_LAND) {
        if (!bsValueBoolean(left)) {
            return left;
        }
        bsRelease(left);
        return bsEvalExpr(expr->u.binary.right, options, scope, builtins, script, statement);
    }
    if (op == BS_BINARY_LOR) {
        if (bsValueBoolean(left)) {
            return left;
        }
        bsRelease(left);
        return bsEvalExpr(expr->u.binary.right, options, scope, builtins, script, statement);
    }

    BSValue right = bsEvalExpr(expr->u.binary.right, options, scope, builtins, script, statement);
    bool bothNumber = (left.type == BS_NUMBER && right.type == BS_NUMBER);
    BSValue result = bsNull();

    switch (op) {
    case BS_BINARY_ADD:
        if (bothNumber) {
            result = bsArithmetic(left.u.number + right.u.number);
        } else if (left.type == BS_STRING || right.type == BS_STRING) {
            result = bsStringConcat(left, right);
        } else if (left.type == BS_DATETIME && right.type == BS_NUMBER) {
            double value = (double) left.u.datetime + right.u.number;
            result = (isfinite(value) && fabs(value) <= BS_DATETIME_MAX) ? bsDatetime((int64_t) value) : bsNull();
        } else if (left.type == BS_NUMBER && right.type == BS_DATETIME) {
            double value = left.u.number + (double) right.u.datetime;
            result = (isfinite(value) && fabs(value) <= BS_DATETIME_MAX) ? bsDatetime((int64_t) value) : bsNull();
        }
        break;

    case BS_BINARY_SUB:
        if (bothNumber) {
            result = bsArithmetic(left.u.number - right.u.number);
        } else if (left.type == BS_DATETIME && right.type == BS_DATETIME) {
            result = bsNumber((double) (left.u.datetime - right.u.datetime));
        }
        break;

    case BS_BINARY_MUL:
        if (bothNumber) {
            result = bsArithmetic(left.u.number * right.u.number);
        }
        break;

    case BS_BINARY_DIV:
        if (bothNumber) {
            result = bsArithmetic(left.u.number / right.u.number);
        }
        break;

    case BS_BINARY_MOD:
        if (bothNumber) {
            result = bsArithmetic(fmod(left.u.number, right.u.number));
        }
        break;

    case BS_BINARY_EXP:
        if (bothNumber) {
            result = bsArithmetic(pow(left.u.number, right.u.number));
        }
        break;

    case BS_BINARY_LT:
        result = bsBoolean(bothNumber ? left.u.number < right.u.number : bsValueCompare(left, right) < 0);
        break;

    case BS_BINARY_LTE:
        result = bsBoolean(bothNumber ? left.u.number <= right.u.number : bsValueCompare(left, right) <= 0);
        break;

    case BS_BINARY_GT:
        result = bsBoolean(bothNumber ? left.u.number > right.u.number : bsValueCompare(left, right) > 0);
        break;

    case BS_BINARY_GTE:
        result = bsBoolean(bothNumber ? left.u.number >= right.u.number : bsValueCompare(left, right) >= 0);
        break;

    case BS_BINARY_EQ:
        result = bsBoolean(bothNumber ? left.u.number == right.u.number : bsValueCompare(left, right) == 0);
        break;

    case BS_BINARY_NE:
        result = bsBoolean(bothNumber ? left.u.number != right.u.number : bsValueCompare(left, right) != 0);
        break;

    default:
        /* The bitwise operators - these coerce to 32-bit integers, as JavaScript does */
        if (bsIsInteger(left) && bsIsInteger(right)) {
            int32_t leftInt = bsToInt32(left.u.number);
            int32_t rightInt = bsToInt32(right.u.number);
            uint32_t shift = ((uint32_t) rightInt) & 31u;
            switch (op) {
            case BS_BINARY_AND:
                result = bsNumber((double) (leftInt & rightInt));
                break;
            case BS_BINARY_OR:
                result = bsNumber((double) (leftInt | rightInt));
                break;
            case BS_BINARY_XOR:
                result = bsNumber((double) (leftInt ^ rightInt));
                break;
            case BS_BINARY_SHL:
                result = bsNumber((double) (int32_t) ((uint32_t) leftInt << shift));
                break;
            default:
                result = bsNumber((double) (leftInt >> shift));
                break;
            }
        }
        break;
    }

    bsRelease(left);
    bsRelease(right);
    return result;
}


static BSValue bsEvalExpr(BSExpr *expr, BSOptions *options, BSScope *scope, bool builtins,
                          BSScript *script, BSStatement *statement)
{
    switch (expr->type) {
    case BS_EXPR_NUMBER:
        return bsNumber(expr->u.number);

    case BS_EXPR_STRING:
        return bsRetain(expr->u.string);

    case BS_EXPR_VARIABLE:
        switch (expr->u.variable.special) {
        case BS_SPECIAL_NULL:
            return bsNull();
        case BS_SPECIAL_TRUE:
            return bsBoolean(true);
        case BS_SPECIAL_FALSE:
            return bsBoolean(false);
        default:
            return bsRetain(bsLookup(expr->u.variable.name, expr->u.variable.slot, options, scope));
        }

    default:
        break;
    }

    /* The recursive expression kinds are depth-guarded */
    if (++options->depth > options->depthMax) {
        options->depth--;
        bsErrorSetStatement(options, script, statement, "Maximum expression depth exceeded");
        return bsNull();
    }

    BSValue result;
    switch (expr->type) {
    case BS_EXPR_FUNCTION:
        result = bsEvalFunction(expr, options, scope, builtins, script, statement);
        break;

    case BS_EXPR_BINARY:
        result = bsEvalBinary(expr, options, scope, builtins, script, statement);
        break;

    case BS_EXPR_UNARY: {
        BSValue value = bsEvalExpr(expr->u.unary.expr, options, scope, builtins, script, statement);
        switch (expr->u.unary.op) {
        case BS_UNARY_NOT:
            result = bsBoolean(!bsValueBoolean(value));
            break;
        case BS_UNARY_NEG:
            result = value.type == BS_NUMBER ? bsNumber(-value.u.number) : bsNull();
            break;
        default:
            result = bsIsInteger(value) ? bsNumber((double) ~bsToInt32(value.u.number)) : bsNull();
            break;
        }
        bsRelease(value);
        break;
    }

    default:
        result = bsEvalExpr(expr->u.group, options, scope, builtins, script, statement);
        break;
    }

    options->depth--;
    return result;
}


BSValue bsEvaluateExpression(BSExpr *expr, BSOptions *options, BSScope *scope, bool builtins)
{
    return bsEvalExpr(expr, options, scope, builtins, NULL, NULL);
}


BSValue bsEvaluateExpressionModel(BSValue exprModel, BSOptions *options, BSValue locals, bool builtins)
{
    BSExpr *expr = bsExprFromModel(exprModel);
    if (expr == NULL) {
        return bsNull();
    }
    BSScope scope;
    bsScopeInit(&scope);
    scope.object = locals;
    BSValue result = bsEvalExpr(expr, options, &scope, builtins, NULL, NULL);
    bsExprFree(expr);
    return result;
}


/*
 * Statement coverage
 */


static void bsRecordCoverage(BSScript *script, BSStatement *statement, BSValue coverage)
{
    if (script->scriptName.type != BS_STRING || statement->lineNumber == 0) {
        return;
    }

    BSValue scripts = bsObjectGet(coverage, "scripts");
    if (scripts.type != BS_OBJECT) {
        scripts = bsObjectNew();
        bsObjectSet(coverage, "scripts", scripts);
    }
    BSValue scriptCoverage = bsObjectGetString(scripts, script->scriptName);
    if (scriptCoverage.type != BS_OBJECT) {
        scriptCoverage = bsObjectNew();
        bsObjectSet(scriptCoverage, "script", bsScriptToModel(script));
        bsObjectSet(scriptCoverage, "covered", bsObjectNew());
        bsObjectSetString(scripts, script->scriptName, scriptCoverage);
    }

    BSValue covered = bsObjectGet(scriptCoverage, "covered");
    char lineKey[16];
    snprintf(lineKey, sizeof(lineKey), "%d", statement->lineNumber);
    BSValue coveredStatement = bsObjectGet(covered, lineKey);
    if (coveredStatement.type != BS_OBJECT) {
        coveredStatement = bsObjectNew();
        bsObjectSet(coveredStatement, "statement", bsStatementToModel(statement));
        bsObjectSet(coveredStatement, "count", bsNumber(0));
        bsObjectSet(covered, lineKey, coveredStatement);
    }
    BSValue count = bsObjectGet(coveredStatement, "count");
    bsObjectSet(coveredStatement, "count", bsNumber(count.u.number + 1));
}


/*
 * Script functions
 */


static void bsScriptFunctionFree(void *data)
{
    BSScriptFunction *scriptFunction = data;
    bsScriptRelease(scriptFunction->script);
    free(scriptFunction);
}


BSValue bsScriptFunctionCall(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSScriptFunction *scriptFunction = data;
    BSFunctionDef *def = scriptFunction->def;

    BSScope scope;
    bsScopeInit(&scope);
    BSValue slotsInline[16];
    BSValue *slots = def->slotCount <= 16 ? slotsInline : bsAlloc(def->slotCount * sizeof(BSValue));
    for (size_t ix = 0; ix < def->slotCount; ix++) {
        slots[ix] = bsUnset();
    }
    scope.slots = slots;
    scope.slotCount = def->slotCount;

    /* Bind the declared arguments */
    size_t ixArgLast = def->argCount - 1;
    for (size_t ix = 0; ix < def->argCount; ix++) {
        if (def->lastArgArray && ix == ixArgLast) {
            BSValue rest = bsArrayNewCapacity(argCount > ix ? argCount - ix : 0);
            for (size_t ixRest = ix; ixRest < argCount; ixRest++) {
                bsArrayPush(rest, bsRetain(args[ixRest]));
            }
            slots[ix] = rest;
        } else {
            slots[ix] = ix < argCount ? bsRetain(args[ix]) : bsNull();
        }
    }

    BSValue result = bsExecuteStatements(scriptFunction->script, def->statements, def->statementCount,
                                         options, &scope);
    for (size_t ix = 0; ix < def->slotCount; ix++) {
        if (!BS_IS_UNSET(slots[ix])) {
            bsRelease(slots[ix]);
        }
    }
    if (slots != slotsInline) {
        free(slots);
    }
    return result;
}


/*
 * Statement execution
 */


static bool bsExecuteInclude(BSScript *script, BSStatement *statement, BSOptions *options);


BSValue bsExecuteStatements(BSScript *script, BSStatement **statements, size_t statementCount,
                            BSOptions *options, BSScope *scope)
{
    /* The coverage configuration is invariant across this call */
    BSValue coverage = bsObjectGet(options->globals, BS_GLOBAL_COVERAGE);
    bool hasCoverage = coverage.type == BS_OBJECT && bsValueBoolean(bsObjectGet(coverage, "enabled")) &&
        !script->system;

    size_t ixStatement = 0;
    while (ixStatement < statementCount) {
        BSStatement *statement = statements[ixStatement];

        options->statementCount++;
        if (options->maxStatements > 0 && options->statementCount > options->maxStatements) {
            bsErrorSetStatement(options, script, statement, "Exceeded maximum script statements (%lld)",
                                (long long) options->maxStatements);
            return bsNull();
        }
        if (hasCoverage) {
            bsRecordCoverage(script, statement, coverage);
        }

        switch (statement->type) {
        case BS_STMT_EXPR: {
            BSValue value = bsEvalExpr(statement->u.expr.expr, options, scope, false, script, statement);
            if (statement->u.expr.name.type == BS_STRING) {
                /*
                 * A function body always has a slot for every name it assigns, so an assignment
                 * either targets a slot or - at the script's top level - a global
                 */
                if (scope != NULL && statement->u.expr.slot >= 0) {
                    BSValue previous = scope->slots[statement->u.expr.slot];
                    scope->slots[statement->u.expr.slot] = value;
                    if (!BS_IS_UNSET(previous)) {
                        bsRelease(previous);
                    }
                } else {
                    bsObjectSetString(options->globals, statement->u.expr.name, value);
                }
            } else {
                bsRelease(value);
            }
            break;
        }

        case BS_STMT_JUMP: {
            bool jump = true;
            if (statement->u.jump.expr != NULL) {
                BSValue value = bsEvalExpr(statement->u.jump.expr, options, scope, false, script, statement);
                jump = bsValueBoolean(value);
                bsRelease(value);
            }
            if (jump) {
                if (statement->u.jump.index < 0) {
                    bsErrorSetStatement(options, script, statement, "Unknown jump label \"%s\"",
                                        bsStringData(statement->u.jump.label));
                    return bsNull();
                }
                ixStatement = (size_t) statement->u.jump.index;
                if (hasCoverage) {
                    bsRecordCoverage(script, statements[ixStatement], coverage);
                }
            }
            break;
        }

        case BS_STMT_RETURN: {
            BSValue result = statement->u.ret.expr != NULL ?
                bsEvalExpr(statement->u.ret.expr, options, scope, false, script, statement) : bsNull();
            return result;
        }

        case BS_STMT_FUNCTION: {
            BSScriptFunction *scriptFunction = bsAlloc(sizeof(BSScriptFunction));
            scriptFunction->script = bsScriptRetain(script);
            scriptFunction->def = statement->u.function.def;
            BSValue function = bsFunctionNew(bsStringData(statement->u.function.def->name),
                                             bsScriptFunctionCall, scriptFunction, bsScriptFunctionFree);
            bsObjectSetString(options->globals, statement->u.function.def->name, function);
            break;
        }

        case BS_STMT_LABEL:
            break;

        default:
            if (!bsExecuteInclude(script, statement, options)) {
                return bsNull();
            }
            break;
        }

        if (options->error.type == BS_STRING) {
            return bsNull();
        }
        ixStatement++;
    }

    return bsNull();
}


static bool bsExecuteInclude(BSScript *script, BSStatement *statement, BSOptions *options)
{
    for (size_t ix = 0; ix < statement->u.include.count; ix++) {
        const BSInclude *include = &statement->u.include.includes[ix];
        bool system = include->system;

        /* Fix up the non-system include URL */
        BSValue includeUrl = bsRetain(include->url);
        if (!system && options->urlFn != NULL) {
            char *resolved = options->urlFn(bsStringData(includeUrl), options->urlData);
            bsAssign(&includeUrl, bsStringNew(resolved));
            free(resolved);
        }

        /* System include keys are bracketed so they cannot collide with local include URLs */
        BSValue includeKey = system ? bsStringNewFormat("<%s>", bsStringData(includeUrl)) : bsRetain(includeUrl);
        BSValue includes = bsObjectGet(options->globals, BS_GLOBAL_INCLUDES);
        if (includes.type != BS_OBJECT) {
            includes = bsObjectNew();
            bsObjectSet(options->globals, BS_GLOBAL_INCLUDES, includes);
        }
        if (bsValueBoolean(bsObjectGetString(includes, includeKey))) {
            bsRelease(includeKey);
            bsRelease(includeUrl);
            continue;
        }
        bsObjectSetString(includes, includeKey, bsBoolean(true));
        bsRelease(includeKey);

        /* Get the include script text */
        char *includeText = NULL;
        size_t includeSize = 0;
        if (system) {
            const char *systemText = bsSystemIncludeGet(bsStringData(includeUrl));
            if (systemText != NULL) {
                includeSize = strlen(systemText);
                includeText = bsAlloc(includeSize + 1);
                memcpy(includeText, systemText, includeSize + 1);
            }
        } else if (options->fetchFn != NULL) {
            BSFetchRequest request;
            memset(&request, 0, sizeof(request));
            request.url = bsStringData(includeUrl);
            request.headers = bsNull();
            includeText = options->fetchFn(&request, &includeSize, options->fetchData);
        }
        if (includeText == NULL) {
            bsErrorSetStatement(options, script, statement, "Include of \"%s\" failed", bsStringData(includeUrl));
            bsRelease(includeUrl);
            return false;
        }

        /* Parse the include script */
        BSParserError parserError;
        memset(&parserError, 0, sizeof(parserError));
        BSScript *includeScript = bsParseScript(includeText, includeSize, 1, bsStringData(includeUrl),
                                                &parserError);
        free(includeText);
        if (includeScript == NULL) {
            BSValue message = bsRetain(parserError.message);
            bsErrorSet(options, "%s", bsStringData(message));
            bsRelease(message);
            bsParserErrorFree(&parserError);
            bsRelease(includeUrl);
            return false;
        }
        includeScript->system = system;

        /* Execute the include script with URLs relative to the include */
        BSUrlFn savedUrlFn = options->urlFn;
        void *savedUrlData = options->urlData;
        void (*savedUrlDataFree)(void *) = options->urlDataFree;
        options->urlFn = bsUrlFileRelative;
        options->urlData = bsStrdup(bsStringData(includeUrl));
        options->urlDataFree = free;
        BSValue result = bsExecuteStatements(includeScript, includeScript->statements,
                                             includeScript->statementCount, options, NULL);
        bsRelease(result);
        free(options->urlData);
        options->urlFn = savedUrlFn;
        options->urlData = savedUrlData;
        options->urlDataFree = savedUrlDataFree;
        bsScriptRelease(includeScript);
        bsRelease(includeUrl);

        if (options->error.type == BS_STRING) {
            return false;
        }
    }
    return true;
}


BSValue bsExecuteScript(BSScript *script, BSOptions *options)
{
    bsLibraryGlobals(options->globals);
    options->statementCount = 0;
    return bsExecuteStatements(script, script->statements, script->statementCount, options, NULL);
}
