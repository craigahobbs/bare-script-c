/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The BareScript runtime
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "barescript/json.h"
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


void bsFunctionError(BSOptions *options, const char *format, ...)
{
    if (options == NULL || options->argsError.type == BS_STRING) {
        return;
    }
    va_list args;
    va_start(args, format);
    options->argsError = bsStringNewVFormat(format, args);
    va_end(args);
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

    /* Search the registered directories, caching what is found. A directory takes precedence over
       the bundled library, so a script can run against an include library checkout. */
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

    /* The bundled include library, as a compiled JSON script model */
    return bsIncludeSource(name);
}


void bsSystemIncludeClear(void)
{
    bsRelease(bsSystemIncludes);
    bsSystemIncludes = bsNull();
    bsRelease(bsSystemIncludePaths);
    bsSystemIncludePaths = bsNull();
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


/*
 * An evaluated value. Leaves (literals and variable lookups) are borrowed from the AST or a
 * slot/global; the caller retains only if the value must outlive the next evaluation. Heap
 * results (concat, calls) are owned.
 */
typedef struct {
    BSValue value;
    bool owned;
} BSEval;

static inline BSEval bsEvalBorrowed(BSValue value)
{
    BSEval eval;
    eval.value = value;
    eval.owned = false;
    return eval;
}

static inline BSEval bsEvalOwned(BSValue value)
{
    BSEval eval;
    eval.value = value;
    eval.owned = true;
    return eval;
}

static inline void bsEvalDrop(BSEval eval)
{
    if (eval.owned) {
        bsRelease(eval.value);
    }
}

static inline BSValue bsEvalTake(BSEval eval)
{
    return eval.owned ? eval.value : bsRetain(eval.value);
}


typedef struct {
    BSOptions *options;
    BSScope *scope;
    BSScript *script;
    BSStatement *statement;
    bool builtins;
} BSEvalCtx;

static BSEval bsEvalExpr(BSExpr *expr, const BSEvalCtx *ctx, int depth);


static bool bsExprPure(const BSExpr *expr)
{
    for (;;) {
        switch (expr->type) {
        case BS_EXPR_NUMBER:
        case BS_EXPR_STRING:
        case BS_EXPR_VARIABLE:
            return true;
        case BS_EXPR_FUNCTION:
            return false;
        case BS_EXPR_BINARY:
            if (!bsExprPure(expr->u.binary.left)) {
                return false;
            }
            expr = expr->u.binary.right;
            break;
        case BS_EXPR_UNARY:
            expr = expr->u.unary.expr;
            break;
        default:
            expr = expr->u.group;
            break;
        }
    }
}


/* Look up a variable in a locals object or the globals. Slot hits are handled by bsEvalExpr. */
static BSValue bsLookup(BSValue name, BSOptions *options, BSScope *scope)
{
    if (scope != NULL && scope->slots == NULL && scope->object.type == BS_OBJECT) {
        BSValue found;
        if (bsObjectLookupString(scope->object, name, &found)) {
            return found;
        }
    }
    return bsObjectGetString(options->globals, name);
}


/* Look up a function, using the call site's globals cache when the name is not a local */
static BSValue bsLookupFunction(BSExpr *expr, const BSEvalCtx *ctx)
{
    BSScope *scope = ctx->scope;
    if (scope != NULL) {
        if (scope->slots != NULL) {
            if (expr->u.function.slot >= 0) {
                BSValue value = scope->slots[expr->u.function.slot];
                if (!BS_IS_UNSET(value)) {
                    return value;
                }
            }
        } else if (scope->object.type == BS_OBJECT) {
            BSValue found;
            if (bsObjectLookupString(scope->object, expr->u.function.name, &found)) {
                return found;
            }
        }
    }

    BSOptions *options = ctx->options;
    if (options->globals.type == BS_OBJECT) {
        BSObject *globals = options->globals.u.object;
        if (expr->u.function.cachedObject == globals &&
            expr->u.function.cachedGen == globals->generation &&
            expr->u.function.cached.type != BS_NULL) {
            return expr->u.function.cached;
        }
        BSValue function = bsObjectGetString(options->globals, expr->u.function.name);
        expr->u.function.cached = function;
        expr->u.function.cachedGen = globals->generation;
        expr->u.function.cachedObject = globals;
        return function;
    }
    return bsNull();
}


static BSEval bsEvalFunction(BSExpr *expr, const BSEvalCtx *ctx, int depth)
{
    /* The built-in "if" function evaluates only the selected branch */
    if (expr->u.function.isIf) {
        size_t argCount = expr->u.function.argCount;
        bool test = false;
        if (argCount >= 1) {
            BSEval value = bsEvalExpr(expr->u.function.args[0], ctx, depth);
            test = bsValueBoolean(value.value);
            bsEvalDrop(value);
        }
        BSExpr *branch = NULL;
        if (test) {
            branch = argCount >= 2 ? expr->u.function.args[1] : NULL;
        } else {
            branch = argCount >= 3 ? expr->u.function.args[2] : NULL;
        }
        return branch != NULL ? bsEvalExpr(branch, ctx, depth) : bsEvalBorrowed(bsNull());
    }

    /* Evaluate the function arguments. A later effectful arg can reassign a borrowed heap value. */
    size_t argCount = expr->u.function.argCount;
    BSValue argsInline[8];
    bool ownedInline[8];
    bool laterInline[8];
    BSValue *args = argsInline;
    bool *owned = ownedInline;
    bool *later = laterInline;
    if (argCount > 8) {
        args = bsAlloc(argCount * sizeof(BSValue));
        owned = bsAlloc(argCount * sizeof(bool));
        later = bsAlloc(argCount * sizeof(bool));
    }
    bool effectful = false;
    for (size_t ix = argCount; ix-- > 0; ) {
        later[ix] = effectful;
        if (!effectful && !bsExprPure(expr->u.function.args[ix])) {
            effectful = true;
        }
    }
    for (size_t ix = 0; ix < argCount; ix++) {
        BSEval eval = bsEvalExpr(expr->u.function.args[ix], ctx, depth);
        if (!eval.owned && eval.value.type >= BS_STRING && later[ix]) {
            eval.value = bsRetain(eval.value);
            eval.owned = true;
        }
        args[ix] = eval.value;
        owned[ix] = eval.owned;
    }

    /* Resolve the function value */
    BSValue function = bsLookupFunction(expr, ctx);
    if (function.type == BS_NULL && ctx->builtins) {
        function = bsLibraryExpressionFunction(expr->u.function.name);
    }

    BSValue result = bsNull();
    BSOptions *options = ctx->options;
    BSScript *script = ctx->script;
    BSStatement *statement = ctx->statement;
    if (function.type == BS_FUNCTION) {
        int savedDepth = options->depth;
        options->depth = depth;
        result = bsFunctionInvoke(function, args, argCount, options);
        options->depth = savedDepth;

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
        if (owned[ix]) {
            bsRelease(args[ix]);
        }
    }
    if (args != argsInline) {
        free(args);
        free(owned);
        free(later);
    }
    return bsEvalOwned(result);
}


static BSEval bsEvalBinary(BSExpr *expr, const BSEvalCtx *ctx, int depth)
{
    BSBinaryOp op = expr->u.binary.op;
    BSEval left = bsEvalExpr(expr->u.binary.left, ctx, depth);

    /* The short-circuiting logical operators */
    if (op == BS_BINARY_LAND) {
        if (!bsValueBoolean(left.value)) {
            return left;
        }
        bsEvalDrop(left);
        return bsEvalExpr(expr->u.binary.right, ctx, depth);
    }
    if (op == BS_BINARY_LOR) {
        if (bsValueBoolean(left.value)) {
            return left;
        }
        bsEvalDrop(left);
        return bsEvalExpr(expr->u.binary.right, ctx, depth);
    }

    /* A later operand can reassign the slot a borrowed heap value came from */
    if (!left.owned && left.value.type >= BS_STRING && !bsExprPure(expr->u.binary.right)) {
        left.value = bsRetain(left.value);
        left.owned = true;
    }

    BSEval right = bsEvalExpr(expr->u.binary.right, ctx, depth);
    bool bothNumber = (left.value.type == BS_NUMBER && right.value.type == BS_NUMBER);
    BSValue result = bsNull();
    bool owned = false;

    switch (op) {
    case BS_BINARY_ADD:
        if (bothNumber) {
            result = bsArithmetic(left.value.u.number + right.value.u.number);
        } else if (left.value.type == BS_STRING || right.value.type == BS_STRING) {
            result = bsStringConcat(left.value, right.value);
            owned = true;
        } else if (left.value.type == BS_DATETIME && right.value.type == BS_NUMBER) {
            double value = (double) left.value.u.datetime + right.value.u.number;
            result = (isfinite(value) && fabs(value) <= BS_DATETIME_MAX) ? bsDatetime((int64_t) value) : bsNull();
        } else if (left.value.type == BS_NUMBER && right.value.type == BS_DATETIME) {
            double value = left.value.u.number + (double) right.value.u.datetime;
            result = (isfinite(value) && fabs(value) <= BS_DATETIME_MAX) ? bsDatetime((int64_t) value) : bsNull();
        }
        break;

    case BS_BINARY_SUB:
        if (bothNumber) {
            result = bsArithmetic(left.value.u.number - right.value.u.number);
        } else if (left.value.type == BS_DATETIME && right.value.type == BS_DATETIME) {
            result = bsNumber((double) (left.value.u.datetime - right.value.u.datetime));
        }
        break;

    case BS_BINARY_MUL:
        if (bothNumber) {
            result = bsArithmetic(left.value.u.number * right.value.u.number);
        }
        break;

    case BS_BINARY_DIV:
        if (bothNumber) {
            result = bsArithmetic(left.value.u.number / right.value.u.number);
        }
        break;

    case BS_BINARY_MOD:
        if (bothNumber) {
            result = bsArithmetic(fmod(left.value.u.number, right.value.u.number));
        }
        break;

    case BS_BINARY_EXP:
        if (bothNumber) {
            result = bsArithmetic(pow(left.value.u.number, right.value.u.number));
        }
        break;

    case BS_BINARY_LT:
        result = bsBoolean(bothNumber ? left.value.u.number < right.value.u.number :
                           bsValueCompare(left.value, right.value) < 0);
        break;

    case BS_BINARY_LTE:
        result = bsBoolean(bothNumber ? left.value.u.number <= right.value.u.number :
                           bsValueCompare(left.value, right.value) <= 0);
        break;

    case BS_BINARY_GT:
        result = bsBoolean(bothNumber ? left.value.u.number > right.value.u.number :
                           bsValueCompare(left.value, right.value) > 0);
        break;

    case BS_BINARY_GTE:
        result = bsBoolean(bothNumber ? left.value.u.number >= right.value.u.number :
                           bsValueCompare(left.value, right.value) >= 0);
        break;

    case BS_BINARY_EQ:
        result = bsBoolean(bothNumber ? left.value.u.number == right.value.u.number :
                           bsValueCompare(left.value, right.value) == 0);
        break;

    case BS_BINARY_NE:
        result = bsBoolean(bothNumber ? left.value.u.number != right.value.u.number :
                           bsValueCompare(left.value, right.value) != 0);
        break;

    default:
        /* The bitwise operators - these coerce to 32-bit integers, as JavaScript does */
        if (bsIsInteger(left.value) && bsIsInteger(right.value)) {
            int32_t leftInt = bsToInt32(left.value.u.number);
            int32_t rightInt = bsToInt32(right.value.u.number);
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

    bsEvalDrop(left);
    bsEvalDrop(right);
    return owned ? bsEvalOwned(result) : bsEvalBorrowed(result);
}


static BSEval bsEvalExpr(BSExpr *expr, const BSEvalCtx *ctx, int depth)
{
    for (;;) {
        switch (expr->type) {
        case BS_EXPR_NUMBER:
            return bsEvalBorrowed(bsNumber(expr->u.number));

        case BS_EXPR_STRING:
            return bsEvalBorrowed(expr->u.string);

        case BS_EXPR_VARIABLE:
            switch (expr->u.variable.special) {
            case BS_SPECIAL_NULL:
                return bsEvalBorrowed(bsNull());
            case BS_SPECIAL_TRUE:
                return bsEvalBorrowed(bsBoolean(true));
            case BS_SPECIAL_FALSE:
                return bsEvalBorrowed(bsBoolean(false));
            default: {
                int slot = expr->u.variable.slot;
                BSScope *scope = ctx->scope;
                if (scope != NULL && scope->slots != NULL && slot >= 0) {
                    BSValue value = scope->slots[slot];
                    if (!BS_IS_UNSET(value)) {
                        return bsEvalBorrowed(value);
                    }
                }
                return bsEvalBorrowed(bsLookup(expr->u.variable.name, ctx->options, scope));
            }
            }

        default:
            break;
        }

        /* The recursive expression kinds are depth-guarded */
        if (depth >= ctx->options->depthMax) {
            bsErrorSetStatement(ctx->options, ctx->script, ctx->statement,
                                "Maximum expression depth exceeded");
            return bsEvalBorrowed(bsNull());
        }
        depth++;

        switch (expr->type) {
        case BS_EXPR_GROUP:
            expr = expr->u.group;
            continue;

        case BS_EXPR_FUNCTION:
            return bsEvalFunction(expr, ctx, depth);

        case BS_EXPR_BINARY:
            return bsEvalBinary(expr, ctx, depth);

        default: {
            BSEval value = bsEvalExpr(expr->u.unary.expr, ctx, depth);
            BSValue result;
            switch (expr->u.unary.op) {
            case BS_UNARY_NOT:
                result = bsBoolean(!bsValueBoolean(value.value));
                break;
            case BS_UNARY_NEG:
                result = value.value.type == BS_NUMBER ? bsNumber(-value.value.u.number) : bsNull();
                break;
            default:
                result = bsIsInteger(value.value) ? bsNumber((double) ~bsToInt32(value.value.u.number)) :
                    bsNull();
                break;
            }
            bsEvalDrop(value);
            return bsEvalBorrowed(result);
        }
        }
    }
}


BSValue bsEvaluateExpression(BSExpr *expr, BSOptions *options, BSScope *scope, bool builtins)
{
    BSEvalCtx ctx;
    ctx.options = options;
    ctx.scope = scope;
    ctx.script = NULL;
    ctx.statement = NULL;
    ctx.builtins = builtins;
    return bsEvalTake(bsEvalExpr(expr, &ctx, options->depth));
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
    BSEvalCtx ctx;
    ctx.options = options;
    ctx.scope = &scope;
    ctx.script = NULL;
    ctx.statement = NULL;
    ctx.builtins = builtins;
    BSValue result = bsEvalTake(bsEvalExpr(expr, &ctx, options->depth));
    bsExprFree(expr);
    return result;
}


/*
 * Statement coverage
 */


static void bsLineKey(int line, char *buf)
{
    char digits[16];
    size_t n = 0;
    unsigned v = (unsigned) line;
    do {
        digits[n++] = (char) ('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    for (size_t ix = 0; ix < n; ix++) {
        buf[ix] = digits[n - 1 - ix];
    }
    buf[n] = '\0';
}


static void bsCoverageReset(BSScript *script)
{
    free(script->coverageCounts);
    script->coverageCounts = NULL;
    script->coverageLineCap = 0;
    script->coverageCovered = bsNull();
}


static void bsCoverageGrow(BSScript *script, int line)
{
    int cap = script->coverageLineCap;
    int needed = line + 1;
    int newCap = cap == 0 ? 32 : cap;
    while (newCap < needed) {
        newCap *= 2;
    }
    script->coverageCounts = bsRealloc(script->coverageCounts, (size_t) newCap * sizeof(BSValue *));
    memset(script->coverageCounts + cap, 0, (size_t) (newCap - cap) * sizeof(BSValue *));
    script->coverageLineCap = newCap;
}


static void bsCoverageEnsure(BSScript *script, BSValue coverage)
{
    if (script->coverageOwner != coverage.u.object) {
        bsCoverageReset(script);
        script->coverageOwner = coverage.u.object;
    }
    if (script->coverageCovered.type == BS_OBJECT) {
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
    script->coverageCovered = bsObjectGet(scriptCoverage, "covered");
}


static void bsRecordCoverage(BSScript *script, BSStatement *statement, BSValue coverage)
{
    int line = statement->lineNumber;
    if (line <= 0 || script->scriptName.type != BS_STRING) {
        return;
    }

    if (script->coverageOwner == coverage.u.object &&
        line < script->coverageLineCap &&
        script->coverageCounts[line] != NULL) {
        script->coverageCounts[line]->u.number += 1;
        return;
    }

    bsCoverageEnsure(script, coverage);
    if (line >= script->coverageLineCap) {
        bsCoverageGrow(script, line);
    }

    char lineKey[16];
    bsLineKey(line, lineKey);
    BSValue coveredStatement = bsObjectGet(script->coverageCovered, lineKey);
    if (coveredStatement.type == BS_OBJECT) {
        script->coverageCounts[line] = bsObjectValuePtr(coveredStatement, "count", 5);
        script->coverageCounts[line]->u.number += 1;
        return;
    }

    coveredStatement = bsObjectNew();
    bsObjectSet(coveredStatement, "statement", bsStatementToModel(statement));
    bsObjectSet(coveredStatement, "count", bsNumber(1));
    bsObjectSet(script->coverageCovered, lineKey, coveredStatement);
    script->coverageCounts[line] = bsObjectValuePtr(coveredStatement, "count", 5);
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
    scope.slots = slots;
    scope.slotCount = def->slotCount;

    /* Bind the declared arguments, then mark remaining locals unset */
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
    for (size_t ix = def->argCount; ix < def->slotCount; ix++) {
        slots[ix] = bsUnset();
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
    /* System includes never record coverage - skip the globals lookup on that hot path */
    BSValue coverage = bsNull();
    bool hasCoverage = false;
    if (!script->system) {
        coverage = bsObjectGet(options->globals, BS_GLOBAL_COVERAGE);
        hasCoverage = coverage.type == BS_OBJECT && bsValueBoolean(bsObjectGet(coverage, "enabled"));
    }

    BSEvalCtx ctx;
    ctx.options = options;
    ctx.scope = scope;
    ctx.script = script;
    ctx.builtins = false;
    ctx.statement = NULL;

    size_t ixStatement = 0;
    while (ixStatement < statementCount) {
        BSStatement *statement = statements[ixStatement];
        ctx.statement = statement;

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
            BSEval eval = bsEvalExpr(statement->u.expr.expr, &ctx, options->depth);
            if (statement->u.expr.name.type == BS_STRING) {
                /*
                 * A function body always has a slot for every name it assigns, so an assignment
                 * either targets a slot or - at the script's top level - a global
                 */
                BSValue value = bsEvalTake(eval);
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
                bsEvalDrop(eval);
            }
            break;
        }

        case BS_STMT_JUMP: {
            bool jump = true;
            if (statement->u.jump.expr != NULL) {
                BSEval eval = bsEvalExpr(statement->u.jump.expr, &ctx, options->depth);
                jump = bsValueBoolean(eval.value);
                bsEvalDrop(eval);
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
            return statement->u.ret.expr != NULL ?
                bsEvalTake(bsEvalExpr(statement->u.ret.expr, &ctx, options->depth)) : bsNull();
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

        /*
         * Load the include script. A system include starting with "{" is the parser-compiled JSON
         * script model - every bundled include is embedded pre-compiled, so including one costs a
         * JSON parse rather than a run of the BareScript parser.
         */
        BSScript *includeScript;
        if (system && includeText[0] == '{') {
            BSValue model = bsJSONDecode(includeText, includeSize, NULL);
            includeScript = bsScriptFromModel(model, bsStringData(includeUrl));
            bsRelease(model);
            free(includeText);
            if (includeScript == NULL) {
                bsErrorSetStatement(options, script, statement, "Include of \"%s\" failed",
                                    bsStringData(includeUrl));
                bsRelease(includeUrl);
                return false;
            }
        } else {
            BSParserError parserError;
            memset(&parserError, 0, sizeof(parserError));
            includeScript = bsParseScript(includeText, includeSize, 1, bsStringData(includeUrl),
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

        /* Run the linter over the include in debug mode */
        if (options->logFn != NULL && options->debug && options->error.type != BS_STRING) {
            BSValue warnings = bsLintScript(includeScript, options->globals);
            size_t warningCount = bsArrayCount(warnings);
            if (warningCount != 0) {
                bsLog(options, "BareScript: Include \"%s\" static analysis... %zu warning%s:",
                      bsStringData(includeUrl), warningCount, warningCount > 1 ? "s" : "");
                for (size_t ixWarning = 0; ixWarning < warningCount; ixWarning++) {
                    BSValue warning = bsValueString(bsArrayGet(warnings, ixWarning));
                    bsLog(options, "BareScript: %s", bsStringData(warning));
                    bsRelease(warning);
                }
            }
            bsRelease(warnings);
        }
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
