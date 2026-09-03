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


/* Run a compiled bytecode chunk. Returns an owned value. */
static BSValue bsRunCode(const BSCode *code, BSScript *script, BSOptions *options, BSScope *scope,
                         bool builtins);


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


static void bsErrorFormat(BSValue *slot, const char *format, va_list args)
{
    if (slot->type == BS_STRING) {
        return;
    }
    *slot = bsStringNewVFormat(format, args);
}


void bsErrorSet(BSOptions *options, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    bsErrorFormat(&options->error, format, args);
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
    if (options == NULL) {
        return;
    }
    va_list args;
    va_start(args, format);
    bsErrorFormat(&options->argsError, format, args);
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


#define BS_STACK_INLINE 16
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


/*
 * The line number of the statement containing the instruction at "pc" - the last statement that
 * begins at or before it. Only error paths need a line, so the interpreter loop does not track one.
 */
static int bsCodeLine(const BSCode *code, size_t pc)
{
    return bsCoverLine(code->coverPcs, code->coverLines, code->coverCount, pc);
}


/*
 * Statement coverage
 */


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


static void bsRecordCoverage(BSScript *script, const BSCode *code, uint32_t index, BSValue coverage)
{
    int line = code->coverLines[index];
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
    snprintf(lineKey, sizeof(lineKey), "%d", line);
    BSValue coveredStatement = bsObjectGet(script->coverageCovered, lineKey);
    if (coveredStatement.type == BS_OBJECT) {
        script->coverageCounts[line] = bsObjectValuePtr(coveredStatement, "count", 5);
        script->coverageCounts[line]->u.number += 1;
        return;
    }

    /* A script that forgot its model borrows the statement models back before recording one */
    BSValue statement = (code->cover != NULL || bsScriptRestoreCover(script)) ? code->cover[index] : bsNull();
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
    bsRecordCoverage(script, code, BS_ARG(prev), coverage);
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


/* The script function implementation */
static BSValue bsScriptFunctionCall(const BSValue *args, size_t argCount, BSOptions *options, void *data)
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
            size_t restCount = argCount > ix ? argCount - ix : 0;
            slots[ix] = restCount != 0 ? bsArrayFromArgs(args + ix, restCount) : bsArrayNew();
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
 * The library intrinsic fast paths
 *
 * Returns true with "*result" set to the owned return value when the arguments are the happy-path
 * shape; false means the caller runs the full library function, which validates the arguments and
 * reports the error.
 */
static inline bool bsIntrinsicIndex(BSValue value, size_t *index)
{
    if (value.type != BS_NUMBER) {
        return false;
    }
    double number = value.u.number;
    if (!isfinite(number) || trunc(number) != number || number < 0) {
        return false;
    }
    *index = (size_t) number;
    return true;
}

static bool bsIntrinsicCall(unsigned char id, const BSValue *args, size_t argCount, BSValue *result)
{
    switch (id) {
    case BS_INTRIN_ARRAY_COPY:
        if (argCount == 1 && args[0].type == BS_ARRAY) {
            *result = bsArrayCopy(args[0]);
            return true;
        }
        return false;
    case BS_INTRIN_ARRAY_GET: {
        size_t index;
        if (argCount == 2 && args[0].type == BS_ARRAY && bsIntrinsicIndex(args[1], &index) &&
            index < args[0].u.array->count) {
            *result = bsRetain(args[0].u.array->values[index]);
            return true;
        }
        return false;
    }
    case BS_INTRIN_ARRAY_LENGTH:
        if (argCount == 1 && args[0].type == BS_ARRAY) {
            *result = bsNumber((double) args[0].u.array->count);
            return true;
        }
        return false;
    case BS_INTRIN_ARRAY_POP:
        if (argCount == 1 && args[0].type == BS_ARRAY && args[0].u.array->count != 0) {
            size_t index = args[0].u.array->count - 1;
            *result = bsRetain(args[0].u.array->values[index]);
            bsArrayDelete(args[0], index);
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
    case BS_INTRIN_ARRAY_SET: {
        size_t index;
        if (argCount == 3 && args[0].type == BS_ARRAY && bsIntrinsicIndex(args[1], &index) &&
            index < args[0].u.array->count) {
            bsArraySet(args[0], index, bsRetain(args[2]));
            *result = bsRetain(args[2]);
            return true;
        }
        return false;
    }
    case BS_INTRIN_ARRAY_NEW:
        *result = bsArrayFromArgs(args, argCount);
        return true;
    case BS_INTRIN_MATH_ABS:
        if (argCount == 1 && args[0].type == BS_NUMBER) {
            *result = bsNumber(fabs(args[0].u.number));
            return true;
        }
        return false;
    case BS_INTRIN_MATH_CEIL:
        if (argCount == 1 && args[0].type == BS_NUMBER) {
            *result = bsNumber(ceil(args[0].u.number));
            return true;
        }
        return false;
    case BS_INTRIN_MATH_FLOOR:
        if (argCount == 1 && args[0].type == BS_NUMBER) {
            *result = bsNumber(floor(args[0].u.number));
            return true;
        }
        return false;
    case BS_INTRIN_MATH_SIGN:
        if (argCount == 1 && args[0].type == BS_NUMBER) {
            double x = args[0].u.number;
            *result = bsNumber(x < 0 ? -1 : (x == 0 ? 0 : 1));
            return true;
        }
        return false;
    case BS_INTRIN_MATH_SQRT:
        if (argCount == 1 && args[0].type == BS_NUMBER && args[0].u.number >= 0) {
            *result = bsNumber(sqrt(args[0].u.number));
            return true;
        }
        return false;
    case BS_INTRIN_OBJECT_COPY:
        if (argCount == 1 && args[0].type == BS_OBJECT) {
            *result = bsObjectCopy(args[0]);
            return true;
        }
        return false;
    case BS_INTRIN_OBJECT_DELETE:
        if (argCount == 2 && args[0].type == BS_OBJECT && args[1].type == BS_STRING) {
            bsObjectDelete(args[0], bsStringData(args[1]));
            *result = bsNull();
            return true;
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
    case BS_INTRIN_OBJECT_HAS:
        if (argCount == 2 && args[0].type == BS_OBJECT && args[1].type == BS_STRING) {
            *result = bsBoolean(bsObjectHasString(args[0], args[1]));
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
    case BS_INTRIN_OBJECT_NEW: {
        for (size_t ix = 0; ix < argCount; ix += 2) {
            if (args[ix].type != BS_STRING) {
                return false;
            }
        }
        BSValue object = bsObjectNew();
        for (size_t ix = 0; ix < argCount; ix += 2) {
            bsObjectSetString(object, args[ix], bsRetain(ix + 1 < argCount ? args[ix + 1] : bsNull()));
        }
        *result = object;
        return true;
    }
    case BS_INTRIN_STRING_ENDS_WITH:
        if (argCount == 2 && args[0].type == BS_STRING && args[1].type == BS_STRING) {
            *result = bsBoolean(bsStringEndsWith(args[0], args[1]));
            return true;
        }
        return false;
    case BS_INTRIN_STRING_LENGTH:
        if (argCount == 1 && args[0].type == BS_STRING) {
            *result = bsNumber((double) args[0].u.string->length);
            return true;
        }
        return false;
    case BS_INTRIN_STRING_STARTS_WITH:
        if (argCount == 2 && args[0].type == BS_STRING && args[1].type == BS_STRING) {
            *result = bsBoolean(bsStringStartsWith(args[0], args[1]));
            return true;
        }
        return false;
    case BS_INTRIN_SYSTEM_BOOLEAN:
        if (argCount == 1) {
            *result = bsBoolean(bsValueBoolean(args[0]));
            return true;
        }
        return false;
    case BS_INTRIN_SYSTEM_TYPE:
        if (argCount == 1) {
            *result = bsSystemTypeName(args[0]);
            return true;
        }
        return false;
    case BS_INTRIN_REGEX_MATCH:
        if (argCount == 2 && args[0].type == BS_REGEX && args[1].type == BS_STRING) {
            *result = bsRegexMatchImpl(args[0], args[1]);
            return true;
        }
        break;
    }
    return false;
}


/*
 * Look up a global name through its site cache. Returns a borrowed value, or a null value if the
 * name is absent.
 */
/* The globals object's value slot for a name site, or NULL if the name is absent */
static inline BSValue *bsGlobalSlot(BSCallCache *cache, BSValue name, BSOptions *options)
{
    if (options->globals.type != BS_OBJECT) {
        return NULL;
    }
    BSObject *globals = options->globals.u.object;
    if (cache->epoch != options->cacheEpoch || cache->gen != globals->generation) {
        cache->slot = bsObjectValuePtrString(options->globals, name);
        cache->gen = globals->generation;
        cache->epoch = options->cacheEpoch;
    }
    return cache->slot;
}


static inline BSValue bsGlobalLookup(BSCallCache *cache, BSValue name, BSOptions *options)
{
    BSValue *slot = bsGlobalSlot(cache, name, options);
    return slot != NULL ? *slot : bsNull();
}


/*
 * Call the function named by a CALL_NAME or CALL_SLOT instruction. "pc" is the call instruction's
 * index, for error line numbers. Returns the owned result.
 */
static BSValue bsCall(const BSCode *code, size_t pc, uint8_t op, uint32_t arg, const BSValue *args,
                      size_t argCount, BSScript *script, BSOptions *options, const BSValue *slots,
                      BSValue locals, bool builtins)
{
    BSValue name;
    BSValue function = bsNull();
    if (op == BS_OP_CALL_SLOT) {
        name = code->slotNames[arg];
        if (slots != NULL) {
            BSValue value = slots[arg];
            if (!BS_IS_UNSET(value)) {
                function = value;
            }
        }
        if (function.type == BS_NULL) {
            /* An unset or null slot falls through to the globals */
            function = bsObjectGetString(options->globals, name);
        }
    } else {
        BSCallCache *cache = &code->caches[arg];
        name = code->constants[cache->nameIndex];
        if (locals.type == BS_OBJECT) {
            BSValue found;
            if (bsObjectLookupString(locals, name, &found)) {
                function = found;
            }
        }
        if (function.type == BS_NULL) {
            function = bsGlobalLookup(cache, name, options);
        }
    }

    if (function.type == BS_NULL && builtins) {
        function = bsLibraryExpressionFunction(name);
    }

    if (function.type == BS_FUNCTION) {
        BSFunction *fn = function.u.function;
        BSValue result;
        if (fn->intrinsic != 0 && bsIntrinsicCall(fn->intrinsic, args, argCount, &result)) {
            return result;
        }
        if (options->depth >= options->depthMax) {
            bsErrorSetStatement(options, script, bsCodeLine(code, pc), "Maximum expression depth exceeded");
            return bsNull();
        }
        options->depth++;
        fn->refcount++; /* the call may reassign the global that holds the function */
        result = fn->fn(args, argCount, options, fn->data);
        bsRelease(function);
        options->depth--;
        if (options->argsError.type == BS_STRING) {
            if (options->debug && options->logFn != NULL) {
                const char *scriptName = (script != NULL && script->scriptName.type == BS_STRING) ?
                    bsStringData(script->scriptName) : "";
                bsLog(options, "%s:%d: BareScript: Function \"%s\" failed with error: %s", scriptName,
                      bsCodeLine(code, pc), bsStringData(name), bsStringData(options->argsError));
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
    bsErrorSetStatement(options, script, bsCodeLine(code, pc), "Undefined function \"%s\"",
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
        goto includeFailed;
    }

    BSScript *includeScript;
    if (system && includeText[0] == '{') {
        BSValue model = bsJSONDecode(includeText, includeSize, NULL);
        includeScript = bsScriptFromModel(model, bsStringData(includeUrl));
        bsRelease(model);
        free(includeOwned);
        if (includeScript == NULL) {
            goto includeFailed;
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

        /* Only coverage reporting reads an include's model - keep it while coverage is recording */
        BSValue coverage = bsObjectGet(options->globals, BS_GLOBAL_COVERAGE);
        if (coverage.type != BS_OBJECT || !bsValueBoolean(bsObjectGet(coverage, "enabled"))) {
            bsScriptForgetModel(includeScript);
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

includeFailed:
    bsErrorSetStatement(options, script, lineNumber, "Include of \"%s\" failed", bsStringData(includeUrl));
    bsRelease(includeUrl);
    return false;
}


/*
 * Run a compiled chunk
 *
 * The value stack is allocated once at the chunk's emit-time maximum depth, so pushes and pops
 * carry no bounds checks; the emitter's stack accounting is the invariant. Slot operands are
 * likewise trusted - LOAD_SLOT and STORE_SLOT are only emitted in a function body, which always
 * runs with its own slot array. A runtime error is detected where it can arise: at entry, after
 * each call, and at the statements that raise one themselves.
 *
 * With GNU C, each handler jumps straight to the next one through a label table, so the branch
 * predictor sees one indirect branch per opcode rather than a single shared switch branch.
 */
#if defined(__GNUC__) || defined(__clang__)
#define BS_THREADED_DISPATCH 1
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-label-as-value"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

/*
 * The binary operator handlers that differ only by their operator. Each opcode keeps its own
 * handler - and, when dispatch is threaded, its own dispatch - so no operator is chosen at run time.
 */
/*
 * The modulo operator - integer operands, the common case by far, take the integer remainder,
 * which agrees with fmod (the sign of the dividend, and a signed zero when the remainder is zero)
 */
static inline double bsModulo(double left, double right)
{
    if (left > -9007199254740992.0 && left < 9007199254740992.0 &&
        right > -9007199254740992.0 && right < 9007199254740992.0 && right != 0 &&
        left == (double) (int64_t) left && right == (double) (int64_t) right) {
        int64_t remainder = (int64_t) left % (int64_t) right;
        return remainder != 0 ? (double) remainder : copysign(0.0, left);
    }
    return fmod(left, right);
}


#define BS_ARITHMETIC(name, expr) \
    BS_CASE(name) { \
        BSValue right = stack[--sp]; \
        BSValue left = stack[--sp]; \
        stack[sp++] = (left.type == BS_NUMBER && right.type == BS_NUMBER) ? bsArithmetic(expr) : bsNull(); \
        bsRelease(left); \
        bsRelease(right); \
    } \
    BS_NEXT()

#define BS_COMPARE(name, test) \
    BS_CASE(name) { \
        BSValue right = stack[--sp]; \
        BSValue left = stack[--sp]; \
        int cmp; \
        if (left.type == BS_NUMBER && right.type == BS_NUMBER) { \
            double ln = left.u.number, rn = right.u.number; \
            cmp = ln < rn ? -1 : (ln > rn ? 1 : 0); \
        } else { \
            cmp = bsValueCompare(left, right); \
            bsRelease(left); \
            bsRelease(right); \
        } \
        stack[sp++] = bsBoolean(test); \
    } \
    BS_NEXT()

/* JavaScript semantics: operands are 32-bit integers, and a shift count is masked to five bits */
#define BS_BITWISE(name, expr) \
    BS_CASE(name) { \
        BSValue right = stack[--sp]; \
        BSValue left = stack[--sp]; \
        if (bsIsInteger(left) && bsIsInteger(right)) { \
            int32_t leftInt = bsToInt32(left.u.number); \
            int32_t rightInt = bsToInt32(right.u.number); \
            stack[sp++] = bsNumber((double) (expr)); \
        } else { \
            stack[sp++] = bsNull(); \
        } \
        bsRelease(left); \
        bsRelease(right); \
    } \
    BS_NEXT()

#define BS_JUMP_IF(name, cond) \
    BS_CASE(name) { \
        BSValue value = stack[--sp]; \
        bool take = (cond); \
        bsRelease(value); \
        if (take) { \
            bsJumpCover(code, arg, script, hasCoverage, coverage); \
            pc = arg; \
        } \
    } \
    BS_NEXT()


static BSValue bsRunCode(const BSCode *code, BSScript *script, BSOptions *options, BSScope *scope,
                  bool builtins)
{
    if (options->error.type == BS_STRING) {
        return bsNull();
    }

    BSValue stackInline[BS_STACK_INLINE];
    BSValue *stack = code->stackMax <= BS_STACK_INLINE ? stackInline :
        bsAlloc(code->stackMax * sizeof(BSValue));
    size_t sp = 0;
    BSValue *slots = scope != NULL ? scope->slots : NULL;
    BSValue locals = (scope != NULL && slots == NULL) ? scope->object : bsNull();

    bool countStatements = script != NULL && !script->system;
    BSValue coverage = bsNull();
    bool hasCoverage = false;
    if (countStatements) {
        /* Interned keys make both lookups pointer comparisons - this runs on every function call */
        static BSValue coverageKey = {BS_NULL, {0}};
        static BSValue enabledKey = {BS_NULL, {0}};
        if (coverageKey.type != BS_STRING) {
            coverageKey = bsStringIntern(BS_GLOBAL_COVERAGE, strlen(BS_GLOBAL_COVERAGE));
            enabledKey = bsStringIntern("enabled", 7);
        }
        if (options->globals.type == BS_OBJECT) {
            BSObject *globals = options->globals.u.object;
            if (options->coverageEpoch != options->cacheEpoch || options->coverageGen != globals->generation) {
                options->coverageSlot = bsObjectValuePtrString(options->globals, coverageKey);
                options->coverageGen = globals->generation;
                options->coverageEpoch = options->cacheEpoch;
            }
            BSValue enabled;
            if (options->coverageSlot != NULL && options->coverageSlot->type == BS_OBJECT &&
                bsObjectLookupString(*options->coverageSlot, enabledKey, &enabled)) {
                coverage = *options->coverageSlot;
                hasCoverage = bsValueBoolean(enabled);
            }
        }
    }

    const uint32_t *insts = code->inst;
    size_t pc = 0;
    uint32_t inst;
    uint8_t op;
    uint32_t arg;

#ifdef BS_THREADED_DISPATCH
    /* Indexed by opcode - the order is the BS_OP_ enumeration's */
    static const void *const dispatch[] = {
        &&op_LOAD_NULL, &&op_LOAD_TRUE, &&op_LOAD_FALSE, &&op_LOAD_CONST, &&op_LOAD_SLOT,
        &&op_LOAD_NAME, &&op_STORE_SLOT, &&op_STORE_NAME, &&op_POP, &&op_DUP, &&op_JUMP,
        &&op_JUMP_FALSE, &&op_JUMP_TRUE, &&op_JUMP_UNDEF, &&op_RETURN, &&op_CALL_NAME, &&op_CALL_SLOT,
        &&op_ADD, &&op_SUB, &&op_MUL, &&op_DIV, &&op_MOD, &&op_POW, &&op_EQ, &&op_NE, &&op_LT,
        &&op_LE, &&op_GT, &&op_GE, &&op_BAND, &&op_BOR, &&op_BXOR, &&op_SHL, &&op_SHR, &&op_NEG,
        &&op_NOT, &&op_BNOT, &&op_FUNCTION, &&op_INCLUDE, &&op_STMT
    };
#define BS_CASE(name) op_##name:
#define BS_NEXT() \
    do { \
        inst = insts[pc++]; \
        op = BS_OP(inst); \
        arg = BS_ARG(inst); \
        goto *dispatch[op]; \
    } while (0)
    BS_NEXT();
#else
#define BS_CASE(name) case BS_OP_##name:
#define BS_NEXT() break
    for (;;) {
        inst = insts[pc++];
        op = BS_OP(inst);
        arg = BS_ARG(inst);
        switch (op) {
#endif
        BS_CASE(LOAD_NULL)
            stack[sp++] = bsNull();
            BS_NEXT();

        BS_CASE(LOAD_TRUE)
            stack[sp++] = bsBoolean(true);
            BS_NEXT();

        BS_CASE(LOAD_FALSE)
            stack[sp++] = bsBoolean(false);
            BS_NEXT();

        BS_CASE(LOAD_CONST)
            stack[sp++] = bsRetain(code->constants[arg]);
            BS_NEXT();

        BS_CASE(LOAD_SLOT) {
            BSValue value = slots[arg];
            if (BS_IS_UNSET(value)) {
                value = bsObjectGetString(options->globals, code->slotNames[arg]);
            }
            stack[sp++] = bsRetain(value);
        }
        BS_NEXT();

        BS_CASE(LOAD_NAME) {
            BSCallCache *cache = &code->caches[arg];
            BSValue name = code->constants[cache->nameIndex];
            BSValue value;
            if (locals.type != BS_OBJECT || !bsObjectLookupString(locals, name, &value)) {
                value = bsGlobalLookup(cache, name, options);
            }
            stack[sp++] = bsRetain(value);
        }
        BS_NEXT();

        BS_CASE(STORE_SLOT) {
            BSValue previous = slots[arg];
            slots[arg] = stack[--sp];
            if (!BS_IS_UNSET(previous)) {
                bsRelease(previous);
            }
        }
        BS_NEXT();

        BS_CASE(STORE_NAME) {
            BSCallCache *cache = &code->caches[arg];
            BSValue name = code->constants[cache->nameIndex];
            BSValue value = stack[--sp];
            BSValue *slot = bsGlobalSlot(cache, name, options);
            if (slot != NULL) {
                /* An existing global updates in place - no slot moves, so every site's cache holds */
                BSValue previous = *slot;
                *slot = value;
                bsRelease(previous);
            } else {
                bsObjectSetString(options->globals, name, value);
            }
        }
        BS_NEXT();

        BS_CASE(POP)
            bsRelease(stack[--sp]);
            BS_NEXT();

        BS_CASE(DUP)
            stack[sp] = bsRetain(stack[sp - 1]);
            sp++;
            BS_NEXT();

        BS_CASE(JUMP)
            bsJumpCover(code, arg, script, hasCoverage, coverage);
            pc = arg;
            BS_NEXT();

        BS_JUMP_IF(JUMP_FALSE, !bsValueBoolean(value));
        BS_JUMP_IF(JUMP_TRUE, bsValueBoolean(value));

        BS_CASE(JUMP_UNDEF) {
            /* A trap past the chunk's return carries the jump statement's line in a data word */
            int line = (pc < code->count && BS_OP(insts[pc]) == BS_OP_ARGC) ? (int) BS_ARG(insts[pc]) :
                bsCodeLine(code, pc - 1);
            bsErrorSetStatement(options, script, line, "Unknown jump label \"%s\"",
                                bsStringData(code->constants[arg]));
            goto fail;
        }

        BS_CASE(RETURN) {
            BSValue result = stack[--sp];
            if (stack != stackInline) {
                free(stack);
            }
            return result;
        }

        BS_CASE(CALL_NAME)
        BS_CASE(CALL_SLOT) {
            size_t callPc = pc - 1;
            size_t argCount = BS_ARG(insts[pc++]);
            BSValue *callArgs = stack + (sp - argCount);
            BSValue result = bsCall(code, callPc, op, arg, callArgs, argCount, script, options, slots,
                                    locals, builtins);
            for (size_t ix = 0; ix < argCount; ix++) {
                bsRelease(callArgs[ix]);
            }
            sp -= argCount;
            stack[sp++] = result;
            if (options->error.type == BS_STRING) {
                goto fail;
            }
        }
        BS_NEXT();

        BS_CASE(ADD) {
            BSValue right = stack[--sp];
            BSValue left = stack[--sp];
            if (left.type == BS_NUMBER && right.type == BS_NUMBER) {
                stack[sp++] = bsArithmetic(left.u.number + right.u.number);
            } else {
                stack[sp++] = bsAddSlow(left, right);
                bsRelease(left);
                bsRelease(right);
            }
        }
        BS_NEXT();

        BS_CASE(SUB) {
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
        }
        BS_NEXT();

        BS_ARITHMETIC(MUL, left.u.number * right.u.number);
        BS_ARITHMETIC(DIV, left.u.number / right.u.number);
        BS_ARITHMETIC(MOD, bsModulo(left.u.number, right.u.number));
        BS_ARITHMETIC(POW, pow(left.u.number, right.u.number));

        BS_COMPARE(EQ, cmp == 0);
        BS_COMPARE(NE, cmp != 0);
        BS_COMPARE(LT, cmp < 0);
        BS_COMPARE(LE, cmp <= 0);
        BS_COMPARE(GT, cmp > 0);
        BS_COMPARE(GE, cmp >= 0);

        BS_BITWISE(BAND, leftInt & rightInt);
        BS_BITWISE(BOR, leftInt | rightInt);
        BS_BITWISE(BXOR, leftInt ^ rightInt);
        BS_BITWISE(SHL, (int32_t) ((uint32_t) leftInt << ((uint32_t) rightInt & 31u)));
        BS_BITWISE(SHR, leftInt >> ((uint32_t) rightInt & 31u));

        BS_CASE(NEG) {
            BSValue value = stack[--sp];
            stack[sp++] = value.type == BS_NUMBER ? bsNumber(-value.u.number) : bsNull();
            bsRelease(value);
        }
        BS_NEXT();

        BS_CASE(NOT) {
            BSValue value = stack[--sp];
            stack[sp++] = bsBoolean(!bsValueBoolean(value));
            bsRelease(value);
        }
        BS_NEXT();

        BS_CASE(BNOT) {
            BSValue value = stack[--sp];
            stack[sp++] = bsIsInteger(value) ? bsNumber((double) ~bsToInt32(value.u.number)) : bsNull();
            bsRelease(value);
        }
        BS_NEXT();

        BS_CASE(FUNCTION) {
            BSFunctionDef *def = script->functions[arg];
            BSScriptFunction *scriptFunction = bsAlloc(sizeof(BSScriptFunction));
            scriptFunction->script = bsScriptRetain(script);
            scriptFunction->def = def;
            BSValue function = bsFunctionNew(bsStringData(def->name), bsScriptFunctionCall,
                                             scriptFunction, bsScriptFunctionFree);
            bsObjectSetString(options->globals, def->name, function);
        }
        BS_NEXT();

        BS_CASE(INCLUDE)
            if (!bsExecuteInclude(script, &code->includes[arg], bsCodeLine(code, pc - 1), options)) {
                goto fail;
            }
            BS_NEXT();

        BS_CASE(STMT)
            if (countStatements) {
                options->statementCount++;
                if (options->maxStatements > 0 && options->statementCount > options->maxStatements) {
                    bsErrorSetStatement(options, script, code->coverLines[arg],
                                        "Exceeded maximum script statements (%lld)",
                                        (long long) options->maxStatements);
                    goto fail;
                }
                if (hasCoverage) {
                    bsRecordCoverage(script, code, arg, coverage);
                }
            }
            BS_NEXT();

#ifndef BS_THREADED_DISPATCH
        default: /* GCOV_EXCL_LINE - emit never produces an unknown opcode */
            break; /* GCOV_EXCL_LINE */
        }
    }
#endif
#undef BS_CASE
#undef BS_NEXT

fail:
    while (sp != 0) {
        bsRelease(stack[--sp]);
    }
    if (stack != stackInline) {
        free(stack); /* GCOV_EXCL_LINE - errors are raised at statement boundaries */
    }
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

