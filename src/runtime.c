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

static uint32_t bsCacheEpoch;

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
    options->cacheEpoch = ++bsCacheEpoch;
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


void bsErrorSetStatement(BSOptions *options, const BSScript *script, int lineNumber,
                         const char *format, ...)
{
    if (options->error.type == BS_STRING) {
        return;
    }
    va_list args;
    va_start(args, format);
    BSValue message = bsStringNewVFormat(format, args);
    va_end(args);

    if (script != NULL && script->scriptName.type == BS_STRING) {
        const char *scriptName = bsStringData(script->scriptName);
        if (lineNumber != 0) {
            options->error = bsStringNewFormat("%s:%d: %s", scriptName, lineNumber, bsStringData(message));
        } else {
            options->error = bsStringNewFormat("%s: %s", scriptName, bsStringData(message));
        }
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
 * Bytecode interpreter
 */


#define BS_STACK_INLINE 8
#define BS_SLOTS_INLINE 32


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


static BSValue *bsStackGrow(BSValue *stack, BSValue *inlineBuf, size_t *cap)
{
    size_t old = *cap;
    *cap = old * 2;
    if (stack == inlineBuf) {
        BSValue *heap = bsAlloc(*cap * sizeof(BSValue));
        memcpy(heap, inlineBuf, old * sizeof(BSValue));
        return heap;
    }
    return bsRealloc(stack, *cap * sizeof(BSValue));
}


static BSValue bsLookupName(BSValue name, BSOptions *options, BSScope *scope)
{
    if (scope != NULL && scope->slots == NULL && scope->object.type == BS_OBJECT) {
        BSValue found;
        if (bsObjectLookupString(scope->object, name, &found)) {
            return found;
        }
    }
    return bsObjectGetString(options->globals, name);
}


static BSValue bsAddSlow(BSValue left, BSValue right)
{
    if (left.type == BS_STRING || right.type == BS_STRING) {
        return bsStringConcat(left, right);
    }
    if (left.type == BS_DATETIME && right.type == BS_NUMBER) {
        double value = (double) left.u.datetime + right.u.number;
        return (isfinite(value) && fabs(value) <= BS_DATETIME_MAX) ? bsDatetime((int64_t) value) : bsNull();
    }
    if (left.type == BS_NUMBER && right.type == BS_DATETIME) {
        double value = left.u.number + (double) right.u.datetime;
        return (isfinite(value) && fabs(value) <= BS_DATETIME_MAX) ? bsDatetime((int64_t) value) : bsNull();
    }
    return bsNull();
}


static BSValue bsBitwise(uint8_t op, BSValue left, BSValue right)
{
    if (!bsIsInteger(left) || !bsIsInteger(right)) {
        return bsNull();
    }
    int32_t leftInt = bsToInt32(left.u.number);
    int32_t rightInt = bsToInt32(right.u.number);
    uint32_t shift = ((uint32_t) rightInt) & 31u;
    switch (op) {
    case BS_OP_BAND:
        return bsNumber((double) (leftInt & rightInt));
    case BS_OP_BOR:
        return bsNumber((double) (leftInt | rightInt));
    case BS_OP_BXOR:
        return bsNumber((double) (leftInt ^ rightInt));
    case BS_OP_SHL:
        return bsNumber((double) (int32_t) ((uint32_t) leftInt << shift));
    default:
        return bsNumber((double) (leftInt >> shift));
    }
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


static void bsRecordCoverage(BSScript *script, BSValue statement, int line, BSValue coverage)
{
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
    bsObjectSet(coveredStatement, "statement", bsRetain(statement));
    bsObjectSet(coveredStatement, "count", bsNumber(1));
    bsObjectSet(script->coverageCovered, lineKey, coveredStatement);
    script->coverageCounts[line] = bsObjectValuePtr(coveredStatement, "count", 5);
}


static void bsJumpCover(const BSCode *code, uint32_t target, BSScript *script, bool hasCoverage,
                        BSValue coverage)
{
    if (!hasCoverage || target == 0) {
        return;
    }
    uint32_t prev = code->inst[target - 1];
    if (BS_OP(prev) != BS_OP_STMT) {
        return;
    }
    uint32_t index = BS_ARG(prev);
    bsRecordCoverage(script, code->cover[index], code->coverLines[index], coverage);
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


static bool bsExecuteInclude(BSScript *script, const BSInclude *include, int lineNumber,
                             BSOptions *options);


BSValue bsScriptFunctionCall(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSScriptFunction *scriptFunction = data;
    BSFunctionDef *def = scriptFunction->def;
    size_t slotCount = def->code.slotCount;

    BSScope scope;
    bsScopeInit(&scope);
    BSValue slotsInline[BS_SLOTS_INLINE];
    BSValue *slots = slotCount <= BS_SLOTS_INLINE ? slotsInline : bsAlloc(slotCount * sizeof(BSValue));
    scope.slots = slots;
    scope.slotCount = slotCount;

    size_t ixArgLast = def->argCount != 0 ? def->argCount - 1 : 0;
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
    for (size_t ix = def->argCount; ix < slotCount; ix++) {
        slots[ix] = bsUnset();
    }

    BSValue result = bsRunCode(&def->code, scriptFunction->script, options, &scope, false);
    for (size_t ix = 0; ix < slotCount; ix++) {
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
 * Happy-path library calls that dominate the include-test profile. Returns true if *result is the
 * owned return value; false means the caller should run the full invoke (validation, overrides).
 */
static bool bsCallIntrinsic(unsigned char id, const BSValue *args, size_t argCount, BSValue *result)
{
    switch (id) {
    case BS_INTRIN_ARRAY_GET:
        if (argCount == 2 && args[0].type == BS_ARRAY && args[1].type == BS_NUMBER) {
            double number = args[1].u.number;
            if (isfinite(number) && trunc(number) == number && number >= 0) {
                size_t index = (size_t) number;
                if (index < args[0].u.array->count) {
                    *result = bsRetain(args[0].u.array->values[index]);
                    return true;
                }
            }
        }
        return false;
    case BS_INTRIN_OBJECT_GET:
        if (argCount >= 2 && argCount <= 3 && args[0].type == BS_OBJECT && args[1].type == BS_STRING) {
            BSValue found;
            if (bsObjectLookupString(args[0], args[1], &found)) {
                *result = bsRetain(found);
            } else {
                *result = argCount >= 3 ? bsRetain(args[2]) : bsNull();
            }
            return true;
        }
        return false;
    case BS_INTRIN_ARRAY_NEW: {
        BSValue array = bsArrayNewCapacity(argCount);
        for (size_t ix = 0; ix < argCount; ix++) {
            bsArrayPush(array, bsRetain(args[ix]));
        }
        *result = array;
        return true;
    }
    case BS_INTRIN_OBJECT_NEW:
        for (size_t ix = 0; ix < argCount; ix += 2) {
            if (args[ix].type != BS_STRING) {
                return false;
            }
        }
        {
            BSValue object = bsObjectNew();
            for (size_t ix = 0; ix < argCount; ix += 2) {
                bsObjectSetString(object, args[ix],
                                  bsRetain(ix + 1 < argCount ? args[ix + 1] : bsNull()));
            }
            *result = object;
            return true;
        }
    case BS_INTRIN_ARRAY_LENGTH:
        if (argCount == 1 && args[0].type == BS_ARRAY) {
            *result = bsNumber((double) args[0].u.array->count);
            return true;
        }
        return false;
    case BS_INTRIN_STRING_LENGTH:
        if (argCount == 1 && args[0].type == BS_STRING) {
            *result = bsNumber((double) args[0].u.string->length);
            return true;
        }
        return false;
    case BS_INTRIN_OBJECT_HAS:
        if (argCount == 2 && args[0].type == BS_OBJECT && args[1].type == BS_STRING) {
            *result = bsBoolean(bsObjectHasString(args[0], args[1]));
            return true;
        }
        return false;
    case BS_INTRIN_ARRAY_PUSH:
        if (argCount >= 1 && args[0].type == BS_ARRAY) {
            for (size_t ix = 1; ix < argCount; ix++) {
                bsArrayPush(args[0], bsRetain(args[ix]));
            }
            *result = bsRetain(args[0]);
            return true;
        }
        return false;
    case BS_INTRIN_OBJECT_KEYS:
        if (argCount == 1 && args[0].type == BS_OBJECT) {
            *result = bsObjectKeys(args[0]);
            return true;
        }
        return false;
    case BS_INTRIN_OBJECT_SET:
        if (argCount == 3 && args[0].type == BS_OBJECT && args[1].type == BS_STRING) {
            bsObjectSetString(args[0], args[1], bsRetain(args[2]));
            *result = bsRetain(args[2]);
            return true;
        }
        return false;
    case BS_INTRIN_REGEX_MATCH:
        if (argCount == 2 && args[0].type == BS_REGEX && args[1].type == BS_STRING) {
            *result = bsRegexMatchImpl(args[0], args[1]);
            return true;
        }
        return false;
    default:
        return false;
    }
}


static BSValue bsCall(const BSCode *code, size_t callPc, uint8_t op, uint32_t arg, BSValue *args,
                      size_t argCount, BSScript *script, BSOptions *options, BSScope *scope,
                      bool builtins, int stmtLine)
{
    BSValue name;
    BSValue function = bsNull();
    if (op == BS_OP_CALL_SLOT) {
        uint32_t slot = arg;
        if (scope != NULL && scope->slots != NULL && slot < scope->slotCount) {
            BSValue value = scope->slots[slot];
            if (!BS_IS_UNSET(value)) {
                function = value;
            }
        }
        name = slot < code->slotCount ? code->slotNames[slot] : bsNull();
        if (function.type == BS_NULL && name.type == BS_STRING) {
            function = bsLookupName(name, options, scope);
        }
    } else {
        uint32_t nameIndex = arg;
        name = nameIndex < code->constantCount ? code->constants[nameIndex] : bsNull();
        if (scope != NULL) {
            if (scope->slots == NULL && scope->object.type == BS_OBJECT) {
                BSValue found;
                if (name.type == BS_STRING && bsObjectLookupString(scope->object, name, &found)) {
                    function = found;
                }
            }
        }
        if (function.type == BS_NULL && options->globals.type == BS_OBJECT && name.type == BS_STRING) {
            BSObject *globals = options->globals.u.object;
            BSCallCache *cache = (code->caches != NULL && callPc < code->cacheCount) ?
                &code->caches[callPc] : NULL;
            if (cache != NULL && cache->epoch == options->cacheEpoch &&
                cache->gen == globals->generation && cache->cached.type != BS_NULL) {
                function = cache->cached;
            } else {
                function = bsObjectGetString(options->globals, name);
                if (cache != NULL) {
                    cache->cached = function;
                    cache->gen = globals->generation;
                    cache->epoch = options->cacheEpoch;
                }
            }
        }
    }

    if (function.type == BS_NULL && builtins && name.type == BS_STRING) {
        function = bsLibraryExpressionFunction(name);
    }

    if (function.type == BS_FUNCTION) {
        if (options->depth >= options->depthMax) {
            bsErrorSetStatement(options, script, stmtLine, "Maximum expression depth exceeded");
            return bsNull();
        }
        options->depth++;
        BSValue result;
        unsigned char id = function.u.function->intrinsic;
        if (id != 0 && bsCallIntrinsic(id, args, argCount, &result)) {
            options->depth--;
            return result;
        }
        result = bsFunctionInvoke(function, args, argCount, options);
        options->depth--;
        if (options->argsError.type == BS_STRING) {
            if (options->debug && options->logFn != NULL) {
                const char *scriptName = (script != NULL && script->scriptName.type == BS_STRING) ?
                    bsStringData(script->scriptName) : "";
                bsLog(options, "%s:%d: BareScript: Function \"%s\" failed with error: %s", scriptName,
                      stmtLine, bsStringData(name), bsStringData(options->argsError));
            }
            bsAssign(&options->argsError, bsNull());
        }
        return result;
    }
    if (function.type != BS_NULL) {
        if (options->debug) {
            bsLog(options, "BareScript: Function \"%s\" failed with error: not a function",
                  bsStringData(name));
        }
        return bsNull();
    }
    bsErrorSetStatement(options, script, stmtLine, "Undefined function \"%s\"",
                        bsStringData(name));
    return bsNull();
}


static bool bsExecuteInclude(BSScript *script, const BSInclude *include, int lineNumber,
                             BSOptions *options)
{
    bool system = include->system;

    BSValue includeUrl = bsRetain(include->url);
    if (!system && options->urlFn != NULL) {
        char *resolved = options->urlFn(bsStringData(includeUrl), options->urlData);
        bsAssign(&includeUrl, bsStringNew(resolved));
        free(resolved);
    }

    BSValue includeKey = system ? bsStringNewFormat("<%s>", bsStringData(includeUrl)) : bsRetain(includeUrl);
    BSValue includes = bsObjectGet(options->globals, BS_GLOBAL_INCLUDES);
    if (includes.type != BS_OBJECT) {
        includes = bsObjectNew();
        bsObjectSet(options->globals, BS_GLOBAL_INCLUDES, includes);
    }
    if (bsValueBoolean(bsObjectGetString(includes, includeKey))) {
        bsRelease(includeKey);
        bsRelease(includeUrl);
        return true;
    }
    bsObjectSetString(includes, includeKey, bsBoolean(true));
    bsRelease(includeKey);

    if (system && bsArrayCount(bsSystemIncludePaths) == 0 &&
        (bsSystemIncludes.type != BS_OBJECT ||
         bsObjectGet(bsSystemIncludes, bsStringData(includeUrl)).type != BS_STRING)) {
        BSScript *cached = bsIncludeScript(bsStringData(includeUrl));
        if (cached != NULL) {
            BSValue result = bsRunCode(&cached->code, cached, options, NULL, false);
            bsRelease(result);
            bsScriptRelease(cached);
            bsRelease(includeUrl);
            if (options->error.type == BS_STRING) {
                return false; /* GCOV_EXCL_LINE - bundled includes are trusted */
            }
            return true;
        }
    }

    const char *includeText = NULL;
    char *includeOwned = NULL;
    size_t includeSize = 0;
    if (system) {
        includeText = bsSystemIncludeGet(bsStringData(includeUrl));
        if (includeText != NULL) {
            includeSize = strlen(includeText);
        }
    } else if (options->fetchFn != NULL) {
        BSFetchRequest request;
        memset(&request, 0, sizeof(request));
        request.url = bsStringData(includeUrl);
        request.headers = bsNull();
        includeOwned = options->fetchFn(&request, &includeSize, options->fetchData);
        includeText = includeOwned;
    }
    if (includeText == NULL) {
        bsErrorSetStatement(options, script, lineNumber, "Include of \"%s\" failed",
                            bsStringData(includeUrl));
        bsRelease(includeUrl);
        return false;
    }

    BSScript *includeScript;
    if (system && includeText[0] == '{') {
        BSValue model = bsJSONDecode(includeText, includeSize, NULL);
        includeScript = bsScriptFromModel(model, bsStringData(includeUrl));
        bsRelease(model);
        free(includeOwned);
        if (includeScript == NULL) {
            bsErrorSetStatement(options, script, lineNumber, "Include of \"%s\" failed",
                                bsStringData(includeUrl));
            bsRelease(includeUrl);
            return false;
        }
    } else { /* GCOV_EXCL_LINE - llvm-cov attributes this brace to the JSON-model branch */
        BSParserError parserError;
        memset(&parserError, 0, sizeof(parserError));
        includeScript = bsParseScript(includeText, includeSize, 1, bsStringData(includeUrl),
                                      &parserError);
        free(includeOwned);
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

    BSUrlFn savedUrlFn = options->urlFn;
    void *savedUrlData = options->urlData;
    void (*savedUrlDataFree)(void *) = options->urlDataFree;
    options->urlFn = bsUrlFileRelative;
    options->urlData = bsStrdup(bsStringData(includeUrl));
    options->urlDataFree = free;
    BSValue result = bsRunCode(&includeScript->code, includeScript, options, NULL, false);
    bsRelease(result);

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

    return options->error.type != BS_STRING;
}


BSValue bsRunCode(const BSCode *code, BSScript *script, BSOptions *options, BSScope *scope,
                  bool builtins)
{
    BSValue stackInline[BS_STACK_INLINE];
    BSValue *stack = stackInline;
    size_t stackCap = BS_STACK_INLINE;
    size_t sp = 0;

    bool countStatements = script != NULL && !script->system;
    BSValue coverage = bsNull();
    bool hasCoverage = false;
    if (countStatements) {
        coverage = bsObjectGet(options->globals, BS_GLOBAL_COVERAGE);
        hasCoverage = coverage.type == BS_OBJECT && bsValueBoolean(bsObjectGet(coverage, "enabled"));
    }

    int stmtLine = 0;
    size_t pc = 0;
    const uint32_t *insts = code->inst;
    size_t count = code->count;

    while (pc < count) {
        uint32_t inst = insts[pc];
        uint8_t op = BS_OP(inst);
        uint32_t arg = BS_ARG(inst);
        pc++;

        switch (op) {
        case BS_OP_LOAD_NULL:
            if (sp == stackCap) {
                stack = bsStackGrow(stack, stackInline, &stackCap);
            }
            stack[sp++] = bsNull();
            break;

        case BS_OP_LOAD_TRUE:
            if (sp == stackCap) {
                stack = bsStackGrow(stack, stackInline, &stackCap);
            }
            stack[sp++] = bsBoolean(true);
            break;

        case BS_OP_LOAD_FALSE:
            if (sp == stackCap) {
                stack = bsStackGrow(stack, stackInline, &stackCap);
            }
            stack[sp++] = bsBoolean(false);
            break;

        case BS_OP_LOAD_CONST:
            if (sp == stackCap) {
                stack = bsStackGrow(stack, stackInline, &stackCap);
            }
            stack[sp++] = bsRetain(code->constants[arg]);
            break;

        case BS_OP_LOAD_SLOT: {
            BSValue value = bsNull();
            if (scope != NULL && scope->slots != NULL && arg < scope->slotCount) {
                value = scope->slots[arg];
                if (BS_IS_UNSET(value)) {
                    value = bsLookupName(code->slotNames[arg], options, scope);
                }
            }
            if (sp == stackCap) {
                stack = bsStackGrow(stack, stackInline, &stackCap);
            }
            stack[sp++] = bsRetain(value);
            break;
        }

        case BS_OP_LOAD_NAME:
            if (sp == stackCap) {
                stack = bsStackGrow(stack, stackInline, &stackCap);
            }
            stack[sp++] = bsRetain(bsLookupName(code->constants[arg], options, scope));
            break;

        case BS_OP_STORE_SLOT: {
            BSValue value = sp != 0 ? stack[--sp] : bsNull();
            if (scope != NULL && scope->slots != NULL && arg < scope->slotCount) {
                BSValue previous = scope->slots[arg];
                scope->slots[arg] = value;
                if (!BS_IS_UNSET(previous)) {
                    bsRelease(previous);
                }
            } else { /* GCOV_EXCL_START - STORE_SLOT is only emitted in a function body */
                bsRelease(value);
            } /* GCOV_EXCL_STOP */
            break;
        }

        case BS_OP_STORE_NAME: {
            BSValue value = sp != 0 ? stack[--sp] : bsNull();
            bsObjectSetString(options->globals, code->constants[arg], value);
            break;
        }

        case BS_OP_POP:
            if (sp != 0) {
                bsRelease(stack[--sp]);
            }
            break;

        case BS_OP_DUP:
            if (sp == stackCap) {
                stack = bsStackGrow(stack, stackInline, &stackCap);
            }
            stack[sp] = sp != 0 ? bsRetain(stack[sp - 1]) : bsNull();
            sp++;
            break;

        case BS_OP_JUMP:
            bsJumpCover(code, arg, script, hasCoverage, coverage);
            pc = arg;
            break;

        case BS_OP_JUMP_FALSE: {
            BSValue value = sp != 0 ? stack[--sp] : bsNull();
            bool take = !bsValueBoolean(value);
            bsRelease(value);
            if (take) {
                bsJumpCover(code, arg, script, hasCoverage, coverage);
                pc = arg;
            }
            break;
        }

        case BS_OP_JUMP_TRUE: {
            BSValue value = sp != 0 ? stack[--sp] : bsNull();
            bool take = bsValueBoolean(value);
            bsRelease(value);
            if (take) {
                bsJumpCover(code, arg, script, hasCoverage, coverage);
                pc = arg;
            }
            break;
        }

        case BS_OP_JUMP_UNDEF:
            bsErrorSetStatement(options, script, stmtLine, "Unknown jump label \"%s\"",
                                bsStringData(code->constants[arg]));
            goto fail;

        case BS_OP_RETURN: {
            BSValue result = sp != 0 ? stack[--sp] : bsNull();
            while (sp != 0) { /* GCOV_EXCL_START - emit leaves one value for return */
                bsRelease(stack[--sp]);
            } /* GCOV_EXCL_STOP */
            if (stack != stackInline) {
                free(stack);
            }
            return result;
        }

        case BS_OP_CALL_NAME:
        case BS_OP_CALL_SLOT: {
            size_t callPc = pc - 1;
            size_t argCount = BS_ARG(insts[pc++]);
            BSValue *callArgs = argCount != 0 ? stack + (sp - argCount) : NULL;
            BSValue result = bsCall(code, callPc, op, arg, callArgs, argCount, script, options, scope,
                                    builtins, stmtLine);
            for (size_t ix = 0; ix < argCount; ix++) {
                bsRelease(callArgs[ix]);
            }
            sp -= argCount;
            if (sp == stackCap) {
                stack = bsStackGrow(stack, stackInline, &stackCap);
            }
            stack[sp++] = result;
            break;
        }

        case BS_OP_ADD: {
            BSValue right = stack[--sp];
            BSValue left = stack[--sp];
            if (left.type == BS_NUMBER && right.type == BS_NUMBER) {
                stack[sp++] = bsArithmetic(left.u.number + right.u.number);
            } else {
                stack[sp++] = bsAddSlow(left, right);
                bsRelease(left);
                bsRelease(right);
            }
            break;
        }

        case BS_OP_SUB: {
            BSValue right = stack[--sp];
            BSValue left = stack[--sp];
            if (left.type == BS_NUMBER && right.type == BS_NUMBER) {
                stack[sp++] = bsArithmetic(left.u.number - right.u.number);
            } else if (left.type == BS_DATETIME && right.type == BS_DATETIME) {
                stack[sp++] = bsNumber((double) (left.u.datetime - right.u.datetime));
            } else {
                stack[sp++] = bsNull();
                bsRelease(left);
                bsRelease(right);
            }
            break;
        }

        case BS_OP_MUL: {
            BSValue right = stack[--sp];
            BSValue left = stack[--sp];
            stack[sp++] = (left.type == BS_NUMBER && right.type == BS_NUMBER) ?
                bsArithmetic(left.u.number * right.u.number) : bsNull();
            bsRelease(left);
            bsRelease(right);
            break;
        }

        case BS_OP_DIV: {
            BSValue right = stack[--sp];
            BSValue left = stack[--sp];
            stack[sp++] = (left.type == BS_NUMBER && right.type == BS_NUMBER) ?
                bsArithmetic(left.u.number / right.u.number) : bsNull();
            bsRelease(left);
            bsRelease(right);
            break;
        }

        case BS_OP_MOD: {
            BSValue right = stack[--sp];
            BSValue left = stack[--sp];
            stack[sp++] = (left.type == BS_NUMBER && right.type == BS_NUMBER) ?
                bsArithmetic(fmod(left.u.number, right.u.number)) : bsNull();
            bsRelease(left);
            bsRelease(right);
            break;
        }

        case BS_OP_POW: {
            BSValue right = stack[--sp];
            BSValue left = stack[--sp];
            stack[sp++] = (left.type == BS_NUMBER && right.type == BS_NUMBER) ?
                bsArithmetic(pow(left.u.number, right.u.number)) : bsNull();
            bsRelease(left);
            bsRelease(right);
            break;
        }

        case BS_OP_EQ:
        case BS_OP_NE:
        case BS_OP_LT:
        case BS_OP_LE:
        case BS_OP_GT:
        case BS_OP_GE: {
            BSValue right = stack[--sp];
            BSValue left = stack[--sp];
            int cmp;
            if (left.type == BS_NUMBER && right.type == BS_NUMBER) {
                double ln = left.u.number, rn = right.u.number;
                cmp = ln < rn ? -1 : (ln > rn ? 1 : 0);
            } else {
                cmp = bsValueCompare(left, right);
                bsRelease(left);
                bsRelease(right);
            }
            bool result = false;
            switch (op) {
            case BS_OP_EQ:
                result = cmp == 0;
                break;
            case BS_OP_NE:
                result = cmp != 0;
                break;
            case BS_OP_LT:
                result = cmp < 0;
                break;
            case BS_OP_LE:
                result = cmp <= 0;
                break;
            case BS_OP_GT:
                result = cmp > 0;
                break;
            default:
                result = cmp >= 0;
                break;
            }
            stack[sp++] = bsBoolean(result);
            break;
        }

        case BS_OP_BAND:
        case BS_OP_BOR:
        case BS_OP_BXOR:
        case BS_OP_SHL:
        case BS_OP_SHR: {
            BSValue right = stack[--sp];
            BSValue left = stack[--sp];
            stack[sp++] = bsBitwise(op, left, right);
            bsRelease(left);
            bsRelease(right);
            break;
        }

        case BS_OP_NEG: {
            BSValue value = stack[--sp];
            stack[sp++] = value.type == BS_NUMBER ? bsNumber(-value.u.number) : bsNull();
            bsRelease(value);
            break;
        }

        case BS_OP_NOT: {
            BSValue value = stack[--sp];
            stack[sp++] = bsBoolean(!bsValueBoolean(value));
            bsRelease(value);
            break;
        }

        case BS_OP_BNOT: {
            BSValue value = stack[--sp];
            stack[sp++] = bsIsInteger(value) ? bsNumber((double) ~bsToInt32(value.u.number)) : bsNull();
            bsRelease(value);
            break;
        }

        case BS_OP_FUNCTION: {
            BSFunctionDef *def = script->functions[arg];
            BSScriptFunction *scriptFunction = bsAlloc(sizeof(BSScriptFunction));
            scriptFunction->script = bsScriptRetain(script);
            scriptFunction->def = def;
            BSValue function = bsFunctionNew(bsStringData(def->name), bsScriptFunctionCall,
                                             scriptFunction, bsScriptFunctionFree);
            bsObjectSetString(options->globals, def->name, function);
            break;
        }

        case BS_OP_INCLUDE:
            if (!bsExecuteInclude(script, &code->includes[arg], stmtLine, options)) {
                goto fail;
            }
            break;

        case BS_OP_STMT:
            if (options->error.type == BS_STRING) {
                goto fail;
            }
            stmtLine = code->coverLines[arg];
            if (countStatements) {
                options->statementCount++;
                if (options->maxStatements > 0 && options->statementCount > options->maxStatements) {
                    bsErrorSetStatement(options, script, stmtLine,
                                        "Exceeded maximum script statements (%lld)",
                                        (long long) options->maxStatements);
                    goto fail;
                }
                if (hasCoverage) {
                    bsRecordCoverage(script, code->cover[arg], stmtLine, coverage);
                }
            }
            break;

        default: /* GCOV_EXCL_LINE - emit never produces an unknown opcode */
            break; /* GCOV_EXCL_LINE */
        }
    }

fail:
    while (sp != 0) { /* GCOV_EXCL_START - errors are raised at statement boundaries */
        bsRelease(stack[--sp]);
    } /* GCOV_EXCL_STOP */
    if (stack != stackInline) { /* GCOV_EXCL_START */
        free(stack);
    } /* GCOV_EXCL_STOP */
    return bsNull();
}


BSValue bsEvaluateExpression(BSExpr *expr, BSOptions *options, BSScope *scope, bool builtins)
{
    return bsRunCode(&expr->code, NULL, options, scope, builtins);
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
    BSValue result = bsRunCode(&expr->code, NULL, options, &scope, builtins);
    bsExprFree(expr);
    return result;
}


BSValue bsExecuteScript(BSScript *script, BSOptions *options)
{
    bsLibraryGlobals(options->globals);
    options->statementCount = 0;
    options->cacheEpoch = ++bsCacheEpoch;
    return bsRunCode(&script->code, script, options, NULL, false);
}

