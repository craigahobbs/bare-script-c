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

/* Stamps each options instance so a script's per-site caches re-resolve for a new one */
static _Thread_local uint32_t bsCacheEpoch;

/* The coverage lookup keys, interned once per thread when a coverage slot is first resolved */
static _Thread_local struct {
    BSValue coverage, enabled;
} bsCoverageKeys;

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
    scope->object = bsNull();
}


/*
 * The system include library
 */


static _Thread_local BSValue bsSystemIncludes = {BS_NULL, {0}};
static _Thread_local BSValue bsSystemIncludePaths = {BS_NULL, {0}};


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


/*
 * A registered system include's text, or a search path directory's - registered once found, so a
 * directory takes precedence over the bundled library and a script can run against an include
 * library checkout. NULL if neither has the include.
 */
static const char *bsSystemIncludeText(const char *name)
{
    BSValue text = bsObjectGet(bsSystemIncludes, name);
    if (text.type == BS_STRING) {
        return bsStringData(text);
    }
    for (size_t ix = 0; ix < bsArrayCount(bsSystemIncludePaths); ix++) {
        BSValue directory = bsArrayGet(bsSystemIncludePaths, ix);
        BSValue path = bsStringNewFormat("%s/%s", bsStringData(directory), name);
        BSFetchRequest request = {.url = bsStringData(path), .headers = bsNull()};
        char *fileText = bsFetchReadOnly(&request, NULL, NULL);
        bsRelease(path);
        if (fileText != NULL) {
            bsSystemIncludeRegister(name, fileText);
            free(fileText);
            return bsStringData(bsObjectGet(bsSystemIncludes, name));
        }
    }
    return NULL;
}


const char *bsSystemIncludeGet(const char *name)
{
    const char *text = bsSystemIncludeText(name);
    return text != NULL ? text : bsIncludeSource(name);
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


/* The registers a chunk gets on the C stack before it needs the heap */
#define BS_REGS_INLINE 48


/* A JavaScript ToInt32 of a value bsIsInteger has passed */
static int32_t bsToInt32(double value)
{
    double modulo = fmod(value, 4294967296.0);
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


static void bsCoverageGrow(BSScript *script, int line)
{
    int cap = script->coverageLineCap;
    int newCap = cap == 0 ? 32 : cap;
    while (newCap <= line) {
        newCap *= 2;
    }
    script->coverageCounts = bsRealloc(script->coverageCounts, (size_t) newCap * sizeof(BSValue *));
    memset(script->coverageCounts + cap, 0, (size_t) (newCap - cap) * sizeof(BSValue *));
    script->coverageLineCap = newCap;
}


static void bsCoverageEnsure(BSScript *script, BSValue coverage)
{
    if (script->coverageOwner != coverage.u.object) {
        free(script->coverageCounts);
        script->coverageCounts = NULL;
        script->coverageLineCap = 0;
        script->coverageCovered = bsNull();
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
    if (coveredStatement.type != BS_OBJECT) {
        /* A script that forgot its model borrows the statement models back before recording one */
        BSValue statement = (code->cover != NULL || bsScriptRestoreCover(script)) ? code->cover[index] : bsNull();
        coveredStatement = bsObjectNew();
        bsObjectSet(coveredStatement, "statement", bsRetain(statement));
        bsObjectSet(coveredStatement, "count", bsNumber(0));
        bsObjectSet(script->coverageCovered, lineKey, coveredStatement);
    }
    script->coverageCounts[line] = bsObjectValuePtr(coveredStatement, "count", 5);
    script->coverageCounts[line]->u.number += 1;
}


/*
 * A backward jump lands on the statement it repeats without executing that statement's STMT, so a
 * loop body would be counted once. Only reached when coverage is recording - see BS_JUMP_COVER,
 * which keeps the call itself out of the interpreter's jumps, about a quarter of all dispatches.
 */
static void bsJumpCover(const BSCode *code, uint32_t target, BSScript *script, BSValue coverage)
{
    /*
     * Only a target that lands just past a STMT records anything. Target zero never does - a
     * covered script emits a STMT for its first statement, so every label it can jump back to sits
     * beyond it - and testing for it here is also what keeps the index below from underflowing.
     */
    if (target == 0 || code->inst[target - 1].op != BS_OP_STMT) {
        return;
    }
    bsRecordCoverage(script, code, code->inst[target - 1].a, coverage);
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
    size_t regCount = slotCount + def->code.tempCount;

    /* The registers: the slots, filled from the arguments, then the temporaries, nulled */
    BSValue regsInline[BS_REGS_INLINE];
    BSValue *regs = regCount <= BS_REGS_INLINE ? regsInline : bsAlloc(regCount * sizeof(BSValue));
    BSScope scope = {regs, bsNull()};

    for (size_t ix = 0; ix < def->argCount; ix++) {
        if (def->lastArgArray && ix + 1 == def->argCount) {
            size_t restCount = argCount > ix ? argCount - ix : 0;
            regs[ix] = restCount != 0 ? bsArrayFromArgs(args + ix, restCount) : bsArrayNew();
        } else {
            regs[ix] = ix < argCount ? bsRetain(args[ix]) : bsNull();
        }
    }
    for (size_t ix = def->argCount; ix < slotCount; ix++) {
        regs[ix] = bsUnset();
    }
    for (size_t ix = slotCount; ix < regCount; ix++) {
        regs[ix] = bsNull();
    }

    BSValue result = bsRunCode(&def->code, scriptFunction->script, options, &scope, false);
    for (size_t ix = 0; ix < regCount; ix++) {
        bsReleaseInline(regs[ix]);
    }
    if (regs != regsInline) {
        free(regs);
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

/* An intrinsic whose happy path is one condition and one expression; a miss falls to the function */
#define BS_INTRIN(name, cond, expr) \
    case BS_INTRIN_##name: \
        if (cond) { \
            *result = (expr); \
            return true; \
        } \
        break;

static bool bsIntrinsicCall(unsigned char id, const BSValue *args, size_t argCount, BSValue *result)
{
    switch (id) {
    BS_INTRIN(ARRAY_COPY, argCount == 1 && args[0].type == BS_ARRAY, bsArrayCopy(args[0]))
    case BS_INTRIN_ARRAY_GET: {
        size_t index;
        if (argCount == 2 && args[0].type == BS_ARRAY && bsIntrinsicIndex(args[1], &index) &&
            index < args[0].u.array->count) {
            *result = bsRetain(args[0].u.array->values[index]);
            return true;
        }
        return false;
    }
    BS_INTRIN(ARRAY_LENGTH, argCount == 1 && args[0].type == BS_ARRAY,
              bsNumber((double) args[0].u.array->count))
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
    BS_INTRIN(ARRAY_NEW, true, bsArrayFromArgs(args, argCount))
    BS_INTRIN(MATH_ABS, argCount == 1 && args[0].type == BS_NUMBER, bsNumber(fabs(args[0].u.number)))
    BS_INTRIN(MATH_CEIL, argCount == 1 && args[0].type == BS_NUMBER, bsNumber(ceil(args[0].u.number)))
    BS_INTRIN(MATH_FLOOR, argCount == 1 && args[0].type == BS_NUMBER, bsNumber(floor(args[0].u.number)))
    BS_INTRIN(MATH_SIGN, argCount == 1 && args[0].type == BS_NUMBER,
              bsNumber(args[0].u.number < 0 ? -1 : (args[0].u.number == 0 ? 0 : 1)))
    BS_INTRIN(MATH_SQRT, argCount == 1 && args[0].type == BS_NUMBER && args[0].u.number >= 0,
              bsNumber(sqrt(args[0].u.number)))
    BS_INTRIN(OBJECT_COPY, argCount == 1 && args[0].type == BS_OBJECT, bsObjectCopy(args[0]))
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
    BS_INTRIN(OBJECT_HAS, argCount == 2 && args[0].type == BS_OBJECT && args[1].type == BS_STRING,
              bsBoolean(bsObjectHasString(args[0], args[1])))
    BS_INTRIN(OBJECT_KEYS, argCount == 1 && args[0].type == BS_OBJECT, bsObjectKeys(args[0]))
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
    BS_INTRIN(STRING_ENDS_WITH, argCount == 2 && args[0].type == BS_STRING && args[1].type == BS_STRING,
              bsBoolean(bsStringEndsWith(args[0], args[1])))
    BS_INTRIN(STRING_LENGTH, argCount == 1 && args[0].type == BS_STRING,
              bsNumber((double) args[0].u.string->length))
    BS_INTRIN(STRING_STARTS_WITH, argCount == 2 && args[0].type == BS_STRING && args[1].type == BS_STRING,
              bsBoolean(bsStringStartsWith(args[0], args[1])))
    BS_INTRIN(SYSTEM_BOOLEAN, argCount == 1, bsBoolean(bsValueBoolean(args[0])))
    BS_INTRIN(SYSTEM_TYPE, argCount == 1, bsSystemTypeName(args[0]))
    BS_INTRIN(REGEX_MATCH, argCount == 2 && args[0].type == BS_REGEX && args[1].type == BS_STRING,
              bsRegexMatchImpl(args[0], args[1]))
    }
    return false;
}


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


/* Look up a global name through its site cache. Returns a borrowed value, or a null value if absent. */
static inline BSValue bsGlobalLookup(BSCallCache *cache, BSValue name, BSOptions *options)
{
    BSValue *slot = bsGlobalSlot(cache, name, options);
    return slot != NULL ? *slot : bsNull();
}


/* Call the function named by a CALL_NAME or CALL_SLOT instruction. Returns the owned result. */
static BSValue bsCall(const BSCode *code, const BSInst *inst, const BSValue *args, size_t argCount,
                      BSScript *script, BSOptions *options, const BSValue *regs, BSValue locals,
                      bool builtins)
{
    size_t pc = (size_t) (inst - code->inst);
    BSValue name;
    BSValue function = bsNull();
    if (inst->op == BS_OP_CALL_SLOT) {
        name = code->slotNames[inst->b];
        if (!BS_IS_UNSET(regs[inst->b])) {
            function = regs[inst->b];
        }
        if (function.type == BS_NULL) {
            /* An unset or null slot falls through to the globals */
            function = bsObjectGetString(options->globals, name);
        }
    } else {
        BSCallCache *cache = &code->caches[inst->b];
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
        /*
         * The call may reassign the global that holds the function. A closure's data must outlive
         * the call, so the value is retained; a library function has none, and its value is held by
         * the library table for the thread's lifetime.
         */
        bool retained = fn->data != NULL;
        if (retained) {
            fn->refcount++;
        }
        result = fn->fn(args, argCount, options, fn->data);
        if (retained) {
            bsRelease(function);
        }
        options->depth--;
        if (options->argsError.type == BS_STRING) {
            if (options->debug) {
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

    const char *includeText = NULL;
    char *includeOwned = NULL;
    size_t includeSize = 0;
    if (system) {
        includeText = bsSystemIncludeText(bsStringData(includeUrl));
        if (includeText == NULL) {
            /* The bundled include library's compiled script, cached for the thread */
            BSScript *cached = bsIncludeScript(bsStringData(includeUrl));
            if (cached == NULL) {
                goto includeFailed;
            }
            bsRelease(bsRunCode(&cached->code, cached, options, NULL, false));
            bsScriptRelease(cached);
            bsRelease(includeUrl);
            return options->error.type != BS_STRING;
        }
        includeSize = strlen(includeText);
    } else if (options->fetchFn != NULL) {
        BSFetchRequest request = {.url = bsStringData(includeUrl), .headers = bsNull()};
        includeOwned = options->fetchFn(&request, &includeSize, options->fetchData);
        includeText = includeOwned;
    }
    if (includeText == NULL) {
        goto includeFailed;
    }

    BSScript *includeScript;
    if (system && includeText[0] == '{') {
        /* A registered system include may be a compiled JSON script model */
        BSValue model = bsJSONDecode(includeText, includeSize, NULL);
        includeScript = bsScriptFromModel(model, bsStringData(includeUrl));
        bsRelease(model);
    } else {
        BSParserError parserError = {0};
        includeScript = bsParseScript(includeText, includeSize, 1, bsStringData(includeUrl),
                                      &parserError);
        free(includeOwned);
        if (includeScript == NULL) {
            bsErrorSet(options, "%s", bsStringData(parserError.message));
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
    if (includeScript == NULL) {
        goto includeFailed;
    }
    includeScript->system = system;

    BSUrlFn savedUrlFn = options->urlFn;
    void *savedUrlData = options->urlData;
    void (*savedUrlDataFree)(void *) = options->urlDataFree;
    options->urlFn = bsUrlFileRelative;
    options->urlData = bsStrdup(bsStringData(includeUrl));
    options->urlDataFree = free;
    bsRelease(bsRunCode(&includeScript->code, includeScript, options, NULL, false));

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
 * The registers are allocated once at the chunk's emit-time count, so register operands carry no
 * bounds checks; the emitter's accounting is the invariant. Slot operands are likewise trusted -
 * LOAD_SLOT and CALL_SLOT are only emitted in a function body, which always runs with its own
 * slots. A runtime error is detected where it can arise: at entry, after each call, and at the
 * statements that raise one themselves.
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


/*
 * An operand's value, borrowed: a constant, or a register. The emitter's definite-assignment
 * analysis guarantees a register operand is never the unset marker - a slot that might be unset
 * is read through LOAD_SLOT instead.
 */
static inline BSValue bsOperandRead(const BSCode *code, const BSValue *regs, uint16_t operand)
{
    /* Read as signed: a constant is negative, and a register index is its own sign extension */
    int32_t signedOperand = (int16_t) operand;
    if (signedOperand < 0) {
        return code->constants[signedOperand & 0x7fff];
    }
    return regs[signedOperand];
}


/* Take a temporary's value; retain a slot's or a constant's */
static inline BSValue bsOperandTake(const BSCode *code, BSValue *regs, size_t slotCount, uint16_t operand)
{
    if ((operand & BS_OPERAND_CONST) == 0 && operand >= slotCount) {
        BSValue value = regs[operand];
        regs[operand] = bsNull();
        return value;
    }
    return bsRetain(bsOperandRead(code, regs, operand));
}


/*
 * The binary operator handlers that differ only by their operator. Each opcode keeps its own
 * handler - and, when dispatch is threaded, its own dispatch - so no operator is chosen at run time.
 * Operands are borrowed reads; only the result is owned, and storing it releases what the
 * destination register held.
 */
#define BS_READ(operand) bsOperandRead(code, regs, (operand))

#define BS_ARITHMETIC(name, expr) \
    BS_CASE(name) { \
        BSValue left = BS_READ(inst->b); \
        BSValue right = BS_READ(inst->c); \
        bsAssign(&regs[inst->a], \
                 (left.type == BS_NUMBER && right.type == BS_NUMBER) ? bsArithmetic(expr) : bsNull()); \
    } \
    BS_NEXT()

#define BS_COMPARE(name, test) \
    BS_CASE(name) { \
        BSValue left = BS_READ(inst->b); \
        BSValue right = BS_READ(inst->c); \
        int cmp; \
        if (left.type == BS_NUMBER && right.type == BS_NUMBER) { \
            double ln = left.u.number, rn = right.u.number; \
            cmp = ln < rn ? -1 : (ln > rn ? 1 : 0); \
        } else { \
            cmp = bsValueCompare(left, right); \
        } \
        bsAssign(&regs[inst->a], bsBoolean(test)); \
    } \
    BS_NEXT()

/* JavaScript semantics: operands are 32-bit integers, and a shift count is masked to five bits */
#define BS_BITWISE(name, expr) \
    BS_CASE(name) { \
        BSValue left = BS_READ(inst->b); \
        BSValue right = BS_READ(inst->c); \
        BSValue bits = bsNull(); \
        if (bsIsInteger(left) && bsIsInteger(right)) { \
            int32_t leftInt = bsToInt32(left.u.number); \
            int32_t rightInt = bsToInt32(right.u.number); \
            bits = bsNumber((double) (expr)); \
        } \
        bsAssign(&regs[inst->a], bits); \
    } \
    BS_NEXT()

/* "hasCoverage" is fixed for the whole run, so the branch predicts and the call is not built */
#define BS_JUMP_COVER(target) \
    do { \
        if (hasCoverage) { \
            bsJumpCover(code, (target), script, coverage); \
        } \
    } while (0)

#define BS_JUMP_IF(name, cond) \
    BS_CASE(name) { \
        BSValue value = BS_READ(inst->a); \
        if (cond) { \
            BS_JUMP_COVER(inst->w); \
            pc = inst->w; \
        } \
    } \
    BS_NEXT()


static BSValue bsRunCode(const BSCode *code, BSScript *script, BSOptions *options, BSScope *scope,
                  bool builtins)
{
    if (options->error.type == BS_STRING) {
        return bsNull();
    }

    /*
     * A function call arrives with its registers - the slots the caller filled followed by the
     * temporaries it nulled; a top-level chunk or an expression allocates its own temporaries
     */
    BSValue regsInline[BS_REGS_INLINE];
    size_t slotCount = code->slotCount;
    size_t regCount = slotCount + code->tempCount;
    bool ownRegs = scope == NULL || scope->slots == NULL;
    BSValue *regs = !ownRegs ? scope->slots :
        (regCount <= BS_REGS_INLINE ? regsInline : bsAlloc(regCount * sizeof(BSValue)));
    if (ownRegs) {
        for (size_t ix = 0; ix < regCount; ix++) {
            regs[ix] = bsNull();
        }
    }
    BSValue locals = scope != NULL ? scope->object : bsNull();

    bool countStatements = script != NULL && !script->system;
    BSValue coverage = bsNull();
    bool hasCoverage = false;
    if (countStatements && options->globals.type == BS_OBJECT) {
        /*
         * Interned keys make both lookups pointer comparisons. This runs on every function call, so
         * the thread-local keys are touched only when the slot is re-resolved; the "enabled" lookup
         * that follows a resolved slot finds them interned.
         */
        BSObject *globals = options->globals.u.object;
        if (options->coverageEpoch != options->cacheEpoch || options->coverageGen != globals->generation) {
            if (bsCoverageKeys.coverage.type != BS_STRING) {
                bsCoverageKeys.coverage = bsStringIntern(BS_GLOBAL_COVERAGE, strlen(BS_GLOBAL_COVERAGE));
                bsCoverageKeys.enabled = bsStringIntern("enabled", 7);
            }
            options->coverageSlot = bsObjectValuePtrString(options->globals, bsCoverageKeys.coverage);
            options->coverageGen = globals->generation;
            options->coverageEpoch = options->cacheEpoch;
        }
        BSValue enabled;
        if (options->coverageSlot != NULL && options->coverageSlot->type == BS_OBJECT &&
            bsObjectLookupString(*options->coverageSlot, bsCoverageKeys.enabled, &enabled)) {
            coverage = *options->coverageSlot;
            hasCoverage = bsValueBoolean(enabled);
        }
    }

    const BSInst *insts = code->inst;
    const BSInst *inst;
    size_t pc = 0;
    BSValue result;

#ifdef BS_THREADED_DISPATCH
    /* Indexed by opcode - the order is the BS_OP_ enumeration's */
    static const void *const dispatch[] = {
        &&op_MOVE, &&op_LOAD_NAME, &&op_STORE_NAME, &&op_JUMP, &&op_JUMP_FALSE, &&op_JUMP_TRUE,
        &&op_JUMP_UNDEF, &&op_RETURN, &&op_CALL_NAME, &&op_CALL_SLOT, &&op_ADD, &&op_SUB, &&op_MUL,
        &&op_DIV, &&op_MOD, &&op_POW, &&op_EQ, &&op_NE, &&op_LT, &&op_LE, &&op_GT, &&op_GE,
        &&op_BAND, &&op_BOR, &&op_BXOR, &&op_SHL, &&op_SHR, &&op_NEG, &&op_NOT, &&op_BNOT,
        &&op_FUNCTION, &&op_INCLUDE, &&op_STMT, &&op_LOAD_SLOT
    };
#define BS_CASE(name) op_##name:
#define BS_NEXT() \
    do { \
        inst = &insts[pc++]; \
        goto *dispatch[inst->op]; \
    } while (0)
    BS_NEXT();
#else
#define BS_CASE(name) case BS_OP_##name:
#define BS_NEXT() break
    for (;;) {
        inst = &insts[pc++];
        switch (inst->op) {
#endif
        BS_CASE(MOVE)
            bsAssign(&regs[inst->a], bsOperandTake(code, regs, slotCount, inst->b));
            BS_NEXT();

        BS_CASE(LOAD_SLOT) {
            /* A slot that might not have been assigned yet - unset, it reads the global of its name */
            BSValue value = regs[inst->b];
            if (BS_IS_UNSET(value)) {
                value = bsObjectGetString(options->globals, code->slotNames[inst->b]);
            }
            bsAssign(&regs[inst->a], bsRetain(value));
        }
        BS_NEXT();

        BS_CASE(LOAD_NAME) {
            BSCallCache *cache = &code->caches[inst->b];
            BSValue name = code->constants[cache->nameIndex];
            BSValue value;
            if (locals.type != BS_OBJECT || !bsObjectLookupString(locals, name, &value)) {
                value = bsGlobalLookup(cache, name, options);
            }
            bsAssign(&regs[inst->a], bsRetain(value));
        }
        BS_NEXT();

        BS_CASE(STORE_NAME) {
            BSCallCache *cache = &code->caches[inst->a];
            BSValue name = code->constants[cache->nameIndex];
            BSValue value = bsRetain(BS_READ(inst->b));
            BSValue *slot = bsGlobalSlot(cache, name, options);
            if (slot != NULL) {
                /* An existing global updates in place - no slot moves, so every site's cache holds */
                bsAssign(slot, value);
            } else {
                bsObjectSetString(options->globals, name, value);
            }
        }
        BS_NEXT();

        BS_CASE(JUMP)
            BS_JUMP_COVER(inst->w);
            pc = inst->w;
            BS_NEXT();

        BS_JUMP_IF(JUMP_FALSE, !bsValueBoolean(value));
        BS_JUMP_IF(JUMP_TRUE, bsValueBoolean(value));

        BS_CASE(JUMP_UNDEF) {
            /* A trap past the chunk's return carries the jump statement's line in a data word */
            int line = (pc < code->count && insts[pc].op == BS_OP_DATA) ? (int) insts[pc].w :
                bsCodeLine(code, pc - 1);
            bsErrorSetStatement(options, script, line, "Unknown jump label \"%s\"",
                                bsStringData(code->constants[inst->a]));
            goto fail;
        }

        BS_CASE(RETURN)
            result = bsOperandTake(code, regs, slotCount, inst->a);
            goto done;

        BS_CASE(CALL_NAME)
        BS_CASE(CALL_SLOT) {
            /* The arguments are operands, three per data word, read into a borrowed argument array */
            size_t argCount = inst->c;
            BSValue argsInline[16];
            BSValue *args = argCount <= 16 ? argsInline : bsAlloc(argCount * sizeof(BSValue));
            const BSInst *data = &insts[pc];
            size_t ix = 0;
            for (; ix + BS_OPERANDS_PER_DATA <= argCount; ix += BS_OPERANDS_PER_DATA, data++) {
                args[ix] = BS_READ(data->a);
                args[ix + 1] = BS_READ(data->b);
                args[ix + 2] = BS_READ(data->c);
            }
            if (ix < argCount) {
                args[ix] = BS_READ(data->a);
                if (ix + 1 < argCount) {
                    args[ix + 1] = BS_READ(data->b);
                }
                data++;
            }
            pc = (size_t) (data - insts);
            BSValue value = bsCall(code, inst, args, argCount, script, options, regs, locals, builtins);
            if (args != argsInline) {
                free(args);
            }
            if (inst->a == BS_REG_DISCARD) {
                bsRelease(value);
            } else {
                bsAssign(&regs[inst->a], value);
            }
            if (options->error.type == BS_STRING) {
                goto fail;
            }
        }
        BS_NEXT();

        BS_CASE(ADD) {
            BSValue left = BS_READ(inst->b);
            BSValue right = BS_READ(inst->c);
            bsAssign(&regs[inst->a], (left.type == BS_NUMBER && right.type == BS_NUMBER) ?
                     bsArithmetic(left.u.number + right.u.number) : bsAddSlow(left, right));
        }
        BS_NEXT();

        BS_CASE(SUB) {
            BSValue left = BS_READ(inst->b);
            BSValue right = BS_READ(inst->c);
            BSValue value = bsNull();
            if (left.type == BS_NUMBER && right.type == BS_NUMBER) {
                value = bsArithmetic(left.u.number - right.u.number);
            } else if (left.type == BS_DATETIME && right.type == BS_DATETIME) {
                value = bsNumber((double) (left.u.datetime - right.u.datetime));
            }
            bsAssign(&regs[inst->a], value);
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
            BSValue value = BS_READ(inst->b);
            bsAssign(&regs[inst->a], value.type == BS_NUMBER ? bsNumber(-value.u.number) : bsNull());
        }
        BS_NEXT();

        BS_CASE(NOT)
            bsAssign(&regs[inst->a], bsBoolean(!bsValueBoolean(BS_READ(inst->b))));
            BS_NEXT();

        BS_CASE(BNOT) {
            BSValue value = BS_READ(inst->b);
            bsAssign(&regs[inst->a],
                     bsIsInteger(value) ? bsNumber((double) ~bsToInt32(value.u.number)) : bsNull());
        }
        BS_NEXT();

        BS_CASE(FUNCTION) {
            BSFunctionDef *def = script->functions[inst->a];
            BSScriptFunction *scriptFunction = bsAlloc(sizeof(BSScriptFunction));
            scriptFunction->script = bsScriptRetain(script);
            scriptFunction->def = def;
            BSValue function = bsFunctionNew(bsStringData(def->name), bsScriptFunctionCall,
                                             scriptFunction, bsScriptFunctionFree);
            bsObjectSetString(options->globals, def->name, function);
        }
        BS_NEXT();

        BS_CASE(INCLUDE)
            if (!bsExecuteInclude(script, &code->includes[inst->a], bsCodeLine(code, pc - 1), options)) {
                goto fail;
            }
            BS_NEXT();

        BS_CASE(STMT)
            if (countStatements) {
                options->statementCount++;
                if (options->maxStatements > 0 && options->statementCount > options->maxStatements) {
                    bsErrorSetStatement(options, script, code->coverLines[inst->a],
                                        "Exceeded maximum script statements (%lld)",
                                        (long long) options->maxStatements);
                    goto fail;
                }
                if (hasCoverage) {
                    bsRecordCoverage(script, code, inst->a, coverage);
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
#undef BS_READ

fail:
    result = bsNull();
done:
    if (ownRegs) {
        for (size_t ix = 0; ix < regCount; ix++) {
            bsReleaseInline(regs[ix]);
        }
        if (regs != regsInline) {
            free(regs);
        }
    }
    return result;
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
    BSScope scope = {NULL, locals};
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

