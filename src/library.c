/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The BareScript library
 *
 * Every library function has the same shape: validate the arguments against a static argument
 * model - the BS_ARGS macro - then do the work with the validated values, which are borrowed
 * except for a BS_ARG_LAST_ARRAY argument, which the function releases. bsArgsValidate applies
 * the same coercion and range rules as the reference implementations' value_args_validate,
 * including the documented per-function error return values.
 */

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "barescript/json.h"
#include "barescript/library.h"
#include "barescript/regex.h"

#include "internal.h"


/*
 * Argument validation
 */


static void bsArgsError(BSOptions *options, const char *argName, BSValue argValue)
{
    if (options == NULL || options->argsError.type == BS_STRING) {
        return;
    }
    BSValue json = bsJSONEncode(argValue, 0);
    if (argName == NULL) {
        options->argsError = bsStringNewFormat("Too many arguments (%s)", bsStringData(json));
    } else {
        options->argsError = bsStringNewFormat("Invalid \"%s\" argument value, %s", argName,
                                               bsStringData(json));
    }
    bsRelease(json);
}


/* The value type each argument type requires, indexed by BSArgType; any and boolean never consult it */
static const BSType bsArgValueTypes[] = {
    BS_NULL, BS_NUMBER, BS_STRING, BS_ARRAY, BS_OBJECT, BS_DATETIME, BS_REGEX, BS_FUNCTION, BS_BOOLEAN
};


static bool bsArgLimit(double value, unsigned flags, double limit)
{
    if ((flags & BS_ARG_LT) != 0) {
        return value < limit;
    }
    if ((flags & BS_ARG_LTE) != 0) {
        return value <= limit;
    }
    if ((flags & BS_ARG_GT) != 0) {
        return value > limit;
    }
    if ((flags & BS_ARG_GTE) != 0) {
        return value >= limit;
    }
    return true;
}


/* Report an invalid argument and release the values validated so far. Always returns false. */
static bool bsArgsInvalid(const BSArgModel *argModel, size_t argModelCount, BSValue *values,
                          BSOptions *options, const char *argName, BSValue argValue)
{
    bsArgsError(options, argName, argValue);
    bsArgsFree(argModel, argModelCount, values);
    return false;
}


bool bsArgsValidate(const BSArgModel *argModel, size_t argModelCount, const BSValue *args, size_t argCount,
                    BSValue *values, BSOptions *options)
{
    /* LAST_ARRAY slots are released on failure; every slot must be a valid value first */
    for (size_t ix = 0; ix < argModelCount; ix++) {
        values[ix] = bsNull();
    }
    for (size_t ix = 0; ix < argModelCount; ix++) {
        const BSArgModel *model = &argModel[ix];

        /* The last-argument array collects every remaining argument */
        if ((model->flags & BS_ARG_LAST_ARRAY) != 0) {
            values[ix] = bsArrayFromArgs(args + ix, argCount > ix ? argCount - ix : 0);
            argCount = ix + 1;
            continue;
        }

        /* A missing argument */
        if (ix >= argCount) {
            if ((model->flags & BS_ARG_HAS_DEFAULT) != 0) {
                values[ix] = model->type == BS_ARG_BOOLEAN ? bsBoolean(model->defaultValue != 0) :
                    bsNumber(model->defaultValue);
                continue;
            }
            if (model->type == BS_ARG_BOOLEAN) {
                values[ix] = bsBoolean(false);
                continue;
            }
            if (model->type == BS_ARG_ANY || (model->flags & BS_ARG_NULLABLE) != 0) {
                continue;
            }
            return bsArgsInvalid(argModel, argModelCount, values, options, model->name, bsNull());
        }

        BSValue value = args[ix];

        /* Any type is acceptable */
        if (model->type == BS_ARG_ANY) {
            values[ix] = value;
            continue;
        }

        /* A boolean argument coerces its value */
        if (model->type == BS_ARG_BOOLEAN) {
            values[ix] = bsBoolean(bsValueBoolean(value));
            continue;
        }

        /* A null value */
        if (value.type == BS_NULL) {
            if ((model->flags & BS_ARG_NULLABLE) == 0) {
                return bsArgsInvalid(argModel, argModelCount, values, options, model->name, value);
            }
            continue;
        }

        if (value.type != bsArgValueTypes[model->type]) {
            return bsArgsInvalid(argModel, argModelCount, values, options, model->name, value);
        }
        if (model->type == BS_ARG_NUMBER) {
            double number = value.u.number;
            if (((model->flags & BS_ARG_INTEGER) != 0 && (!isfinite(number) || trunc(number) != number)) ||
                !bsArgLimit(number, model->flags, model->limit) ||
                !bsArgLimit(number, model->flags2, model->limit2)) {
                return bsArgsInvalid(argModel, argModelCount, values, options, model->name, value);
            }
        }
        values[ix] = value;
    }

    /* Extra arguments */
    if (argCount > argModelCount) {
        return bsArgsInvalid(argModel, argModelCount, values, options, NULL, bsNumber((double) argCount));
    }
    return true;
}


void bsArgsFree(const BSArgModel *argModel, size_t argModelCount, BSValue *values)
{
    for (size_t ix = 0; ix < argModelCount; ix++) {
        if ((argModel[ix].flags & BS_ARG_LAST_ARRAY) != 0) {
            bsRelease(values[ix]);
            values[ix] = bsNull();
        }
    }
}


/*
 * A library function's argument prologue: validate "args" against "model", declaring "values" - one
 * validated value per model entry - or return "failValue"
 */
#define BS_ARGS(model, failValue) \
    BSValue values[sizeof(model) / sizeof((model)[0])]; \
    if (!bsArgsValidate(model, sizeof(model) / sizeof((model)[0]), args, argCount, values, options)) { \
        return failValue; \
    }


/* Validate an index argument against "count", declaring "index" - or return null */
#define BS_ARG_INDEX(index, indexValue, count) \
    size_t index; \
    if (!bsArgIndex(indexValue, count, &index, options)) { \
        return bsNull(); \
    }

/* A library function that validates its arguments and returns one expression */
#define BS_LIBRARY_FN(fnName, model, failValue, expression) \
    static BSValue fnName(const BSValue *args, size_t argCount, BSOptions *options, void *data) \
    { \
        BS_ARGS(model, failValue); \
        return expression; \
    }

/* A library function whose one expression takes the raw arguments - it validates them itself, or has none */
#define BS_RAW_FN(fnName, expression) \
    static BSValue fnName(const BSValue *args, size_t argCount, BSOptions *options, void *data) \
    { \
        return expression; \
    }


/* The argument models the one-argument functions share */
static const BSArgModel arrayArgs[] = {{"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0}};
static const BSArgModel objectArgs[] = {{"object", BS_ARG_OBJECT, 0, 0, 0, 0, 0}};
static const BSArgModel stringArgs[] = {{"string", BS_ARG_STRING, 0, 0, 0, 0, 0}};
static const BSArgModel valueArgs[] = {{"value", BS_ARG_ANY, 0, 0, 0, 0, 0}};


BSValue bsStringSlice(BSValue string, size_t begin, size_t end)
{
    const BSString *source = string.u.string;
    if (source->length == source->size) {
        /* An ASCII string's slice is ASCII, at the same offsets */
        return bsStringNewAscii(source->data + begin, end - begin);
    }
    size_t beginOffset = bsStringOffset(string, begin);
    size_t endOffset = bsStringOffset(string, end);
    return bsStringNewSize(bsStringData(string) + beginOffset, endOffset - beginOffset);
}


/* Report an argument error from a function that validates a value itself */
static BSValue bsArgFail(BSOptions *options, const char *argName, BSValue argValue, BSValue errorValue)
{
    bsArgsError(options, argName, argValue);
    return errorValue;
}


/* A non-negative integer index that must be in [0, count). Sets the argument error on failure. */
static bool bsArgIndex(BSValue indexValue, size_t count, size_t *index, BSOptions *options)
{
    if (indexValue.u.number >= (double) count) {
        bsArgsError(options, "index", indexValue);
        return false;
    }
    *index = (size_t) indexValue.u.number;
    return true;
}


/* Slice bounds: start is required, end defaults to count. Both in [0, count]; a reversed range is empty. */
static bool bsArgSlice(BSValue startValue, BSValue endValue, size_t count, size_t *start, size_t *end,
                       BSOptions *options)
{
    double startNumber = startValue.u.number;
    double endNumber = endValue.type == BS_NUMBER ? endValue.u.number : (double) count;
    if (startNumber > (double) count) {
        bsArgsError(options, "start", startValue);
        return false;
    }
    if (endNumber > (double) count) {
        bsArgsError(options, "end", endValue);
        return false;
    }
    *start = (size_t) startNumber;
    *end = endNumber < startNumber ? *start : (size_t) endNumber;
    return true;
}


/* Pop or shift: take the array's last or first value and delete it. Fails if the array is empty. */
static BSValue bsArrayTake(BSValue array, bool last, BSOptions *options)
{
    size_t count = bsArrayCount(array);
    if (count == 0) {
        return bsArgFail(options, "array", array, bsNull());
    }
    size_t index = last ? count - 1 : 0;
    BSValue result = bsRetain(bsArrayGet(array, index));
    bsArrayDelete(array, index);
    return result;
}


/*
 * Array functions
 */


BS_LIBRARY_FN(bsFnArrayCopy, arrayArgs, bsNull(), bsArrayCopy(values[0]))


static const BSArgModel arrayIndexArgs[] = {
    {"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0},
    {"index", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_GTE, 0, 0, 0, 0}
};

static BSValue bsFnArrayDelete(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(arrayIndexArgs, bsNull());
    BS_ARG_INDEX(index, values[1], bsArrayCount(values[0]));
    bsArrayDelete(values[0], index);
    return bsNull();
}


static const BSArgModel arrayExtendArgs[] = {
    {"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0},
    {"array2", BS_ARG_ARRAY, 0, 0, 0, 0, 0}
};

static BSValue bsFnArrayExtend(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(arrayExtendArgs, bsNull());
    size_t count = bsArrayCount(values[1]);
    for (size_t ix = 0; ix < count; ix++) {
        bsArrayPush(values[0], bsRetain(bsArrayGet(values[1], ix)));
    }
    return bsRetain(values[0]);
}


static void bsArrayFlatHelper(BSValue result, BSValue array, int depth, int maxDepth)
{
    if (maxDepth < depth) {
        bsArrayPush(result, bsRetain(array));
        return;
    }
    size_t count = bsArrayCount(array);
    for (size_t ix = 0; ix < count; ix++) {
        BSValue item = bsArrayGet(array, ix);
        if (item.type == BS_ARRAY) {
            bsArrayFlatHelper(result, item, depth + 1, maxDepth);
        } else {
            bsArrayPush(result, bsRetain(item));
        }
    }
}


static const BSArgModel arrayFlatArgs[] = {
    {"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0},
    {"depth", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_HAS_DEFAULT, 10, 0, 0, 0}
};

static BSValue bsFnArrayFlat(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(arrayFlatArgs, bsNull());
    BSValue result = bsArrayNew();
    double depth = values[1].u.number;
    int maxDepth = depth < 0 ? 0 : (depth > 1000000 ? 1000000 : (int) depth);
    bsArrayFlatHelper(result, values[0], 0, maxDepth);
    return result;
}


static BSValue bsFnArrayGet(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(arrayIndexArgs, bsNull());
    BS_ARG_INDEX(index, values[1], bsArrayCount(values[0]));
    return bsRetain(bsArrayGet(values[0], index));
}


/*
 * Search an array from "index" for an item equal to "value", or for which the predicate "value"
 * returns true, forward or backward. Returns the index found, or -1.
 */
static BSValue bsArrayFind(BSValue array, BSValue value, size_t index, bool backward, BSOptions *options)
{
    size_t count = bsArrayCount(array);
    bool isFunction = (value.type == BS_FUNCTION);
    for (size_t ix = index; ix < count; backward ? ix-- : ix++) {
        BSValue item = bsArrayGet(array, ix);
        bool matched;
        if (isFunction) {
            BSValue result = bsFunctionCall(value, &item, 1, options);
            matched = bsValueBoolean(result);
            bsRelease(result);
            if (options->error.type == BS_STRING) {
                return bsNumber(-1);
            }
        } else {
            matched = (bsValueCompare(item, value) == 0);
        }
        if (matched) {
            return bsNumber((double) ix);
        }
    }
    return bsNumber(-1);
}


static const BSArgModel arrayIndexOfArgs[] = {
    {"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0},
    {"value", BS_ARG_ANY, 0, 0, 0, 0, 0},
    {"index", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_HAS_DEFAULT | BS_ARG_GTE, 0, 0, 0, 0}
};

BS_LIBRARY_FN(bsFnArrayIndexOf, arrayIndexOfArgs, bsNumber(-1),
              bsArrayFind(values[0], values[1], (size_t) fmin(values[2].u.number, (double) bsArrayCount(values[0])),
                          false, options))


static const BSArgModel arrayJoinArgs[] = {
    {"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0},
    {"separator", BS_ARG_STRING, 0, 0, 0, 0, 0}
};

static BSValue bsFnArrayJoin(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(arrayJoinArgs, bsNull());
    BSStringBuilder sb;
    bsSBInit(&sb);
    size_t count = bsArrayCount(values[0]);
    for (size_t ix = 0; ix < count; ix++) {
        if (ix != 0) {
            bsSBAppend(&sb, bsStringData(values[1]), bsStringSize(values[1]));
        }
        bsSBAppendValue(&sb, bsArrayGet(values[0], ix));
    }
    return bsSBToValue(&sb);
}


static const BSArgModel arrayLastIndexOfArgs[] = {
    {"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0},
    {"value", BS_ARG_ANY, 0, 0, 0, 0, 0},
    {"index", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_NULLABLE | BS_ARG_GTE, 0, 0, 0, 0}
};

static BSValue bsFnArrayLastIndexOf(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(arrayLastIndexOfArgs, bsNumber(-1));
    size_t count = bsArrayCount(values[0]);
    if (count == 0) {
        return bsNumber(-1);
    }
    size_t index = (values[2].type == BS_NUMBER && values[2].u.number < (double) count) ?
        (size_t) values[2].u.number : count - 1;
    return bsArrayFind(values[0], values[1], index, true, options);
}


BS_LIBRARY_FN(bsFnArrayLength, arrayArgs, bsNumber(0), bsNumber((double) bsArrayCount(values[0])))


BS_RAW_FN(bsFnArrayNew, bsArrayFromArgs(args, argCount))


static const BSArgModel arrayNewSizeArgs[] = {
    {"size", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_HAS_DEFAULT | BS_ARG_GTE, 0, 0, 4294967295, BS_ARG_LTE},
    {"value", BS_ARG_ANY, BS_ARG_HAS_DEFAULT, 0, 0, 0, 0}
};

static BSValue bsFnArrayNewSize(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(arrayNewSizeArgs, bsNull());
    size_t size = (size_t) values[0].u.number;
    BSValue result = bsArrayNewCapacity(size);
    for (size_t ix = 0; ix < size; ix++) {
        bsArrayPush(result, bsRetain(values[1]));
    }
    return result;
}


BS_LIBRARY_FN(bsFnArrayPop, arrayArgs, bsNull(), bsArrayTake(values[0], true, options))


static const BSArgModel arrayPushArgs[] = {
    {"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0},
    {"values", BS_ARG_ANY, BS_ARG_LAST_ARRAY, 0, 0, 0, 0}
};

static BSValue bsFnArrayPush(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(arrayPushArgs, bsNull());
    size_t count = bsArrayCount(values[1]);
    for (size_t ix = 0; ix < count; ix++) {
        bsArrayPush(values[0], bsRetain(bsArrayGet(values[1], ix)));
    }
    bsArgsFree(arrayPushArgs, 2, values);
    return bsRetain(values[0]);
}


static BSValue bsFnArrayReverse(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(arrayArgs, bsNull());
    BSArray *array = values[0].u.array;
    for (size_t ix = 0; ix < array->count / 2; ix++) {
        BSValue swap = array->values[ix];
        array->values[ix] = array->values[array->count - 1 - ix];
        array->values[array->count - 1 - ix] = swap;
    }
    return bsRetain(values[0]);
}


static const BSArgModel arraySetArgs[] = {
    {"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0},
    {"index", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_GTE, 0, 0, 0, 0},
    {"value", BS_ARG_ANY, 0, 0, 0, 0, 0}
};

static BSValue bsFnArraySet(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(arraySetArgs, bsNull());
    BS_ARG_INDEX(index, values[1], bsArrayCount(values[0]));
    bsArraySet(values[0], index, bsRetain(values[2]));
    return bsRetain(values[2]);
}


BS_LIBRARY_FN(bsFnArrayShift, arrayArgs, bsNull(), bsArrayTake(values[0], false, options))


static const BSArgModel arraySliceArgs[] = {
    {"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0},
    {"start", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_HAS_DEFAULT | BS_ARG_GTE, 0, 0, 0, 0},
    {"end", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_NULLABLE | BS_ARG_GTE, 0, 0, 0, 0}
};

static BSValue bsFnArraySlice(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(arraySliceArgs, bsNull());
    size_t start;
    size_t end;
    if (!bsArgSlice(values[1], values[2], bsArrayCount(values[0]), &start, &end, options)) {
        return bsNull();
    }
    BSValue result = bsArrayNewCapacity(end - start);
    for (size_t ix = start; ix < end; ix++) {
        bsArrayPush(result, bsRetain(bsArrayGet(values[0], ix)));
    }
    return result;
}


typedef struct BSSortContext {
    BSValue compareFn;
    BSOptions *options;
} BSSortContext;


static int bsSortCompare(BSValue value1, BSValue value2, void *data)
{
    BSSortContext *context = data;
    if (context->compareFn.type != BS_FUNCTION) {
        return bsValueCompare(value1, value2);
    }
    BSValue compareArgs[2] = {value1, value2};
    BSValue result = bsFunctionCall(context->compareFn, compareArgs, 2, context->options);
    int compare = 0;
    if (result.type == BS_NUMBER) {
        compare = result.u.number < 0 ? -1 : (result.u.number > 0 ? 1 : 0);
    }
    bsRelease(result);
    return compare;
}


static const BSArgModel arraySortArgs[] = {
    {"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0},
    {"compareFn", BS_ARG_FUNCTION, BS_ARG_NULLABLE, 0, 0, 0, 0}
};

static BSValue bsFnArraySort(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(arraySortArgs, bsNull());
    BSSortContext context = {values[1], options};
    bsArraySort(values[0], bsSortCompare, &context);
    return bsRetain(values[0]);
}


/*
 * BareScript functions
 */


static const BSArgModel barescriptEvaluateExpressionArgs[] = {
    {"expr", BS_ARG_OBJECT, 0, 0, 0, 0, 0},
    {"locals", BS_ARG_OBJECT, BS_ARG_NULLABLE, 0, 0, 0, 0},
    {"builtins", BS_ARG_BOOLEAN, BS_ARG_HAS_DEFAULT, 1, 0, 0, 0}
};

BS_LIBRARY_FN(bsFnBarescriptEvaluateExpression, barescriptEvaluateExpressionArgs, bsNull(),
              bsEvaluateExpressionModel(values[0], options, values[1], values[2].u.boolean))


/*
 * Datetime functions
 */


static const BSArgModel datetimeArgs[] = {{"datetime", BS_ARG_DATETIME, 0, 0, 0, 0, 0}};

#define BS_DATETIME_PART(fnName, member) \
    static BSValue fnName(const BSValue *args, size_t argCount, BSOptions *options, void *data) \
    { \
        BS_ARGS(datetimeArgs, bsNull()); \
        BSDatetimeParts parts; \
        bsDatetimeParts(values[0].u.datetime, &parts); \
        return bsNumber(parts.member); \
    }

BS_DATETIME_PART(bsFnDatetimeDay, day)
BS_DATETIME_PART(bsFnDatetimeHour, hour)
BS_DATETIME_PART(bsFnDatetimeMillisecond, millisecond)
BS_DATETIME_PART(bsFnDatetimeMinute, minute)
BS_DATETIME_PART(bsFnDatetimeMonth, month)
BS_DATETIME_PART(bsFnDatetimeSecond, second)
BS_DATETIME_PART(bsFnDatetimeYear, year)


static const BSArgModel datetimeISOFormatArgs[] = {
    {"datetime", BS_ARG_DATETIME, 0, 0, 0, 0, 0},
    {"isDate", BS_ARG_BOOLEAN, 0, 0, 0, 0, 0}
};

static BSValue bsFnDatetimeISOFormat(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(datetimeISOFormatArgs, bsNull());
    if (!values[1].u.boolean) {
        return bsValueString(values[0]);
    }
    BSDatetimeParts parts;
    bsDatetimeParts(values[0].u.datetime, &parts);
    return bsStringNewFormat("%04d-%02d-%02d", parts.year, parts.month, parts.day);
}


static BSValue bsFnDatetimeISOParse(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(stringArgs, bsNull());
    int64_t milliseconds;
    return bsDatetimeParse(bsStringData(values[0]), bsStringSize(values[0]), &milliseconds) ?
        bsDatetime(milliseconds) : bsNull();
}


static const BSArgModel datetimeNewArgs[] = {
    {"year", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_GTE, 0, 100, 0, 0},
    {"month", BS_ARG_NUMBER, BS_ARG_INTEGER, 0, 0, 0, 0},
    {"day", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_GTE, 0, -10000, 10000, BS_ARG_LTE},
    {"hour", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_HAS_DEFAULT, 0, 0, 0, 0},
    {"minute", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_HAS_DEFAULT, 0, 0, 0, 0},
    {"second", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_HAS_DEFAULT, 0, 0, 0, 0},
    {"millisecond", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_HAS_DEFAULT, 0, 0, 0, 0}
};

BS_LIBRARY_FN(bsFnDatetimeNew, datetimeNewArgs, bsNull(),
              bsDatetime(bsDatetimeFromParts(values[0].u.number, values[1].u.number, values[2].u.number,
                                             values[3].u.number, values[4].u.number, values[5].u.number,
                                             values[6].u.number)))


BS_RAW_FN(bsFnDatetimeNow, bsDatetime(bsDatetimeNow()))
BS_RAW_FN(bsFnDatetimeToday, bsDatetime(bsDatetimeToday()))


/*
 * JSON functions
 */


static BSValue bsFnJSONParse(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(stringArgs, bsNull());
    const char *error = NULL;
    size_t errorOffset = 0;
    BSValue result = bsJSONDecodeEx(bsStringData(values[0]), bsStringSize(values[0]), &error,
                                    &errorOffset);
    if (error != NULL) {
        /* Report the decoder's error and its position, as the reference implementations do */
        const char *text = bsStringData(values[0]);
        size_t line = 1;
        size_t column = 1;
        for (size_t ix = 0; ix < errorOffset; ix++) {
            if (text[ix] == '\n') {
                line++;
                column = 1;
            } else {
                column++;
            }
        }
        bsFunctionError(options, "%s: line %zu column %zu (char %zu)", error, line, column,
                        errorOffset);
        return bsNull();
    }
    return result;
}


static const BSArgModel jsonStringifyArgs[] = {
    {"value", BS_ARG_ANY, 0, 0, 0, 0, 0},
    {"indent", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_NULLABLE | BS_ARG_GTE, 0, 1, 2147483647, BS_ARG_LTE}
};

BS_LIBRARY_FN(bsFnJSONStringify, jsonStringifyArgs, bsNull(),
              bsJSONEncode(values[0], values[1].type == BS_NUMBER ? (int) values[1].u.number : 0))


/*
 * Math functions
 */


static const BSArgModel mathXArgs[] = {{"x", BS_ARG_NUMBER, 0, 0, 0, 0, 0}};

/* A function of the number x */
#define BS_MATH_FN(fnName, expression) BS_LIBRARY_FN(fnName, mathXArgs, bsNull(), bsNumber(expression))
#define BS_MATH_X values[0].u.number

BS_MATH_FN(bsFnMathAbs, fabs(BS_MATH_X))
BS_MATH_FN(bsFnMathAcos, acos(BS_MATH_X))
BS_MATH_FN(bsFnMathAsin, asin(BS_MATH_X))
BS_MATH_FN(bsFnMathAtan, atan(BS_MATH_X))
BS_MATH_FN(bsFnMathCeil, ceil(BS_MATH_X))
BS_MATH_FN(bsFnMathCos, cos(BS_MATH_X))
BS_MATH_FN(bsFnMathFloor, floor(BS_MATH_X))
BS_MATH_FN(bsFnMathSign, BS_MATH_X < 0 ? -1 : (BS_MATH_X == 0 ? 0 : 1))
BS_MATH_FN(bsFnMathSin, sin(BS_MATH_X))
BS_MATH_FN(bsFnMathTan, tan(BS_MATH_X))


static const BSArgModel mathAtan2Args[] = {
    {"y", BS_ARG_NUMBER, 0, 0, 0, 0, 0},
    {"x", BS_ARG_NUMBER, 0, 0, 0, 0, 0}
};

BS_LIBRARY_FN(bsFnMathAtan2, mathAtan2Args, bsNull(), bsNumber(atan2(values[0].u.number, values[1].u.number)))


BS_RAW_FN(bsFnMathE, bsNumber(2.718281828459045))
BS_RAW_FN(bsFnMathPi, bsNumber(3.141592653589793))


static const BSArgModel mathLnArgs[] = {{"x", BS_ARG_NUMBER, BS_ARG_GT, 0, 0, 0, 0}};

BS_LIBRARY_FN(bsFnMathLn, mathLnArgs, bsNull(), bsNumber(log(values[0].u.number)))


static const BSArgModel mathLogArgs[] = {
    {"x", BS_ARG_NUMBER, BS_ARG_GT, 0, 0, 0, 0},
    {"base", BS_ARG_NUMBER, BS_ARG_HAS_DEFAULT | BS_ARG_GT, 10, 0, 0, 0}
};

static BSValue bsFnMathLog(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(mathLogArgs, bsNull());
    if (values[1].u.number == 1) {
        return bsArgFail(options, "base", values[1], bsNull());
    }
    return bsNumber(log(values[0].u.number) / log(values[1].u.number));
}


/* The greatest (direction 1) or least (direction -1) argument, or null with no arguments */
static BSValue bsMathExtreme(const BSValue *args, size_t argCount, int direction)
{
    BSValue result = bsNull();
    for (size_t ix = 0; ix < argCount; ix++) {
        if (ix == 0 || bsValueCompare(args[ix], result) * direction > 0) {
            result = args[ix];
        }
    }
    return bsRetain(result);
}


BS_RAW_FN(bsFnMathMax, bsMathExtreme(args, argCount, 1))
BS_RAW_FN(bsFnMathMin, bsMathExtreme(args, argCount, -1))


/* The random number generator - a deterministic xorshift, seeded from the clock and the thread at first use */
static _Thread_local uint64_t bsRandomState;

static BSValue bsFnMathRandom(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    if (bsRandomState == 0) {
        /* The thread-local's address separates threads that start in the same millisecond */
        bsRandomState = ((uint64_t) bsDatetimeNow() ^ (uint64_t) (uintptr_t) &bsRandomState) | 1u;
    }
    uint64_t state = bsRandomState;
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    bsRandomState = state;
    return bsNumber((double) (state >> 11) / (double) (1ULL << 53));
}


static const BSArgModel mathRoundArgs[] = {
    {"x", BS_ARG_NUMBER, 0, 0, 0, 0, 0},
    {"digits", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_HAS_DEFAULT | BS_ARG_GTE, 0, 0, 0, 0}
};

BS_LIBRARY_FN(bsFnMathRound, mathRoundArgs, bsNull(),
              bsNumber(bsNumberRound(values[0].u.number, values[1].u.number)))


static const BSArgModel mathSqrtArgs[] = {{"x", BS_ARG_NUMBER, BS_ARG_GTE, 0, 0, 0, 0}};

BS_LIBRARY_FN(bsFnMathSqrt, mathSqrtArgs, bsNull(), bsNumber(sqrt(values[0].u.number)))


/*
 * Number functions
 */


static BSValue bsFnNumberParseFloat(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(stringArgs, bsNull());
    double number;
    return bsNumberParse(bsStringData(values[0]), bsStringSize(values[0]), &number) ? bsNumber(number) : bsNull();
}


static const BSArgModel numberParseIntArgs[] = {
    {"string", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"radix", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_HAS_DEFAULT | BS_ARG_GTE, 10, 2, 36, BS_ARG_LTE}
};

static BSValue bsFnNumberParseInt(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(numberParseIntArgs, bsNull());
    double number;
    return bsIntegerParse(bsStringData(values[0]), bsStringSize(values[0]), (int) values[1].u.number, &number) ?
        bsNumber(number) : bsNull();
}


static const BSArgModel numberToFixedArgs[] = {
    {"x", BS_ARG_NUMBER, 0, 0, 0, 0, 0},
    {"digits", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_HAS_DEFAULT | BS_ARG_GTE, 2, 0, 100, BS_ARG_LTE},
    {"trim", BS_ARG_BOOLEAN, 0, 0, 0, 0, 0}
};

static BSValue bsFnNumberToFixed(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(numberToFixedArgs, bsNull());
    int digits = (int) values[1].u.number;
    double rounded = bsNumberRound(values[0].u.number, digits);
    if (!isfinite(rounded)) {
        /* Past the double range, the number's own text - "Infinity", as JavaScript's toFixed gives */
        char buffer[32];
        return bsStringNewSize(buffer, bsNumberFormat(rounded, buffer, sizeof(buffer)));
    }
    BSValue result = bsStringNewFormat("%.*f", digits, rounded);
    if (!values[2].u.boolean) {
        return result;
    }

    /* Trim a trailing fraction of only zeroes, along with the decimal point */
    const char *text = bsStringData(result);
    size_t size = bsStringSize(result);
    const char *point = memchr(text, '.', size);
    if (point != NULL) {
        size_t end = size;
        while (end > (size_t) (point - text) && text[end - 1] == '0') {
            end--;
        }
        if (end == (size_t) (point - text) + 1) {
            BSValue trimmed = bsStringNewSize(text, (size_t) (point - text));
            bsRelease(result);
            return trimmed;
        }
    }
    return result;
}


static const BSArgModel numberToStringArgs[] = {
    {"x", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_GTE, 0, 0, 0, 0},
    {"radix", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_HAS_DEFAULT | BS_ARG_GTE, 10, 2, 36, BS_ARG_LTE}
};

/*
 * numberToString's digits are exact for every value its argument model admits, which is more
 * precision than a double holds: 2^1023 divided by ten is not a representable number, so dividing
 * in floating point drifts once the value passes 2^53. The value expands to a base-2^32 magnitude
 * instead, and each pass of the division takes one digit exactly.
 */
#define BS_LIMB_BITS 32
#define BS_LIMB_COUNT (DBL_MAX_EXP / BS_LIMB_BITS) /* a finite double is below 2^DBL_MAX_EXP */

/* Expand a positive integral double into limbs, least significant first. Returns the limb count. */
static size_t bsNumberLimbs(double x, uint32_t *limbs)
{
    /* x is "fraction * 2^exponent" with the fraction in [0.5, 1), so its bit length is "exponent" */
    int exponent;
    uint64_t mantissa = (uint64_t) ldexp(frexp(x, &exponent), DBL_MANT_DIG);
    int shift = exponent - DBL_MANT_DIG;
    if (shift < 0) {
        /* An integer this small has zeros where the mantissa is shifted down */
        mantissa >>= -shift;
        shift = 0;
    }
    size_t count = ((size_t) exponent + BS_LIMB_BITS - 1) / BS_LIMB_BITS;
    size_t offset = (size_t) shift / BS_LIMB_BITS;
    memset(limbs, 0, BS_LIMB_COUNT * sizeof(*limbs));
    limbs[offset] = (uint32_t) mantissa;
    limbs[offset + 1] = (uint32_t) (mantissa >> BS_LIMB_BITS);

    /* The whole limbs of the shift are the offset above; these are the bits left over */
    for (int ix = shift % BS_LIMB_BITS; ix > 0; ix--) {
        uint32_t carry = 0;
        for (size_t limb = offset; limb < count; limb++) {
            uint32_t next = limbs[limb] >> (BS_LIMB_BITS - 1);
            limbs[limb] = (limbs[limb] << 1) | carry;
            carry = next;
        }
    }
    return count;
}


static BSValue bsFnNumberToString(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(numberToStringArgs, bsNull());
    static const char digitChars[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    double x = values[0].u.number;
    uint32_t radix = (uint32_t) values[1].u.number;
    if (x == 0) {
        return bsStringNew("0");
    }

    uint32_t limbs[BS_LIMB_COUNT];
    size_t count = bsNumberLimbs(x, limbs);

    /* The least significant digit comes out first, so the buffer fills from its end. Radix two is
     * the longest form, one digit per bit, so a double's digits always fit. */
    char buffer[DBL_MAX_EXP];
    size_t end = sizeof(buffer);
    while (count != 0) {
        uint32_t remainder = 0;
        for (size_t ix = count; ix > 0; ix--) {
            uint64_t value = ((uint64_t) remainder << BS_LIMB_BITS) | limbs[ix - 1];
            limbs[ix - 1] = (uint32_t) (value / radix);
            remainder = (uint32_t) (value % radix);
        }
        buffer[--end] = digitChars[remainder];
        while (count != 0 && limbs[count - 1] == 0) {
            count--;
        }
    }
    return bsStringNewSize(buffer + end, sizeof(buffer) - end);
}


/*
 * Object functions
 */


static const BSArgModel objectAssignArgs[] = {
    {"object", BS_ARG_OBJECT, 0, 0, 0, 0, 0},
    {"object2", BS_ARG_OBJECT, 0, 0, 0, 0, 0}
};

static BSValue bsFnObjectAssign(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(objectAssignArgs, bsNull());
    bsObjectAssign(values[0], values[1]);
    return bsRetain(values[0]);
}


BS_LIBRARY_FN(bsFnObjectCopy, objectArgs, bsNull(), bsObjectCopy(values[0]))


static const BSArgModel objectKeyArgs[] = {
    {"object", BS_ARG_OBJECT, 0, 0, 0, 0, 0},
    {"key", BS_ARG_STRING, 0, 0, 0, 0, 0}
};

static BSValue bsFnObjectDelete(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(objectKeyArgs, bsNull());
    bsObjectDelete(values[0], bsStringData(values[1]));
    return bsNull();
}


static const BSArgModel objectGetArgs[] = {
    {"object", BS_ARG_OBJECT, 0, 0, 0, 0, 0},
    {"key", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"defaultValue", BS_ARG_ANY, 0, 0, 0, 0, 0}
};

static BSValue bsFnObjectGet(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(objectGetArgs, bsRetain(argCount >= 3 ? args[2] : bsNull()));
    BSValue found;
    return bsRetain(bsObjectLookupString(values[0], values[1], &found) ? found : values[2]);
}


BS_LIBRARY_FN(bsFnObjectHas, objectKeyArgs, bsBoolean(false),
              bsBoolean(bsObjectHasString(values[0], values[1])))


BS_LIBRARY_FN(bsFnObjectKeys, objectArgs, bsNull(), bsObjectKeys(values[0]))


static BSValue bsFnObjectNew(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue result = bsObjectNew();
    for (size_t ix = 0; ix < argCount; ix += 2) {
        BSValue key = args[ix];
        if (key.type != BS_STRING) {
            bsArgsError(options, "keyValues", key);
            bsRelease(result);
            return bsNull();
        }
        bsObjectSetString(result, key, bsRetain(ix + 1 < argCount ? args[ix + 1] : bsNull()));
    }
    return result;
}


static const BSArgModel objectSetArgs[] = {
    {"object", BS_ARG_OBJECT, 0, 0, 0, 0, 0},
    {"key", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"value", BS_ARG_ANY, 0, 0, 0, 0, 0}
};

static BSValue bsFnObjectSet(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(objectSetArgs, bsNull());
    bsObjectSetString(values[0], values[1], bsRetain(values[2]));
    return bsRetain(values[2]);
}


/*
 * Regex functions
 */


BS_LIBRARY_FN(bsFnRegexEscape, stringArgs, bsNull(), bsRegexEscape(values[0]))


/*
 * The thread's regex match model keys - "index", "input", "groups", the empty string, and the group
 * index keys "0", "1", ..., grown to the widest pattern matched. Interned once per thread by
 * bsLibraryInit, and one struct so a match computes the thread-local address once.
 */
static _Thread_local struct {
    BSValue index, input, groups, empty;
    BSValue *group;
    size_t groupCount;
} bsMatchKeys;


static void bsMatchKeyGroupGrow(size_t count)
{
    bsMatchKeys.group = bsRealloc(bsMatchKeys.group, count * sizeof(BSValue));
    for (size_t ix = bsMatchKeys.groupCount; ix < count; ix++) {
        char key[24];
        int keySize = snprintf(key, sizeof(key), "%zu", ix);
        bsMatchKeys.group[ix] = bsStringIntern(key, (size_t) keySize);
    }
    bsMatchKeys.groupCount = count;
}


/* Create a match model object - the "index", "input", and "groups" members */
static BSValue bsRegexMatchModel(BSValue regex, BSValue string, const BSRegexMatch *match)
{
    bool uniqueNames = bsRegexGroupNamesUnique(regex);
    BSValue groups = bsObjectNewCapacity(match->groupCount * (uniqueNames ? 2 : 1));
    if (match->groupCount > bsMatchKeys.groupCount) {
        bsMatchKeyGroupGrow(match->groupCount);
    }
    for (size_t ix = 0; ix < match->groupCount; ix++) {
        BSValue text = bsNull();
        if (match->matched[ix]) {
            const BSRegexSpan *span = &match->groups[ix];
            text = span->end == span->begin ? bsRetain(bsMatchKeys.empty) :
                bsStringSlice(string, span->begin, span->end);
        }
        bsObjectAppend(groups, bsMatchKeys.group[ix], text);

        /* A named group is keyed by both its number and its name - an interned string already. A
         * pattern that reuses a name across alternatives keys the alternative that matched. */
        BSValue name = bsRegexGroupNameValue(regex, ix);
        if (name.type == BS_STRING) {
            if (uniqueNames) {
                bsObjectAppend(groups, name, bsRetain(text));
            } else if (text.type != BS_NULL || !bsObjectHasString(groups, name)) {
                bsObjectSetString(groups, name, bsRetain(text));
            }
        }
    }

    BSValue model = bsObjectNew();
    bsObjectAppend(model, bsMatchKeys.index, bsNumber((double) match->begin));
    bsObjectAppend(model, bsMatchKeys.input, bsRetain(string));
    bsObjectAppend(model, bsMatchKeys.groups, groups);
    return model;
}


BSValue bsRegexMatchImpl(BSValue regex, BSValue string)
{
    BSRegexSubject subject;
    bsRegexSubjectInit(&subject, string);
    BSRegexMatch match;
    BSValue result = bsNull();
    if (bsRegexSearch(regex, &subject, 0, &match)) {
        result = bsRegexMatchModel(regex, string, &match);
    }
    bsRegexSubjectFree(&subject);
    return result;
}


static const BSArgModel regexStringArgs[] = {
    {"regex", BS_ARG_REGEX, 0, 0, 0, 0, 0},
    {"string", BS_ARG_STRING, 0, 0, 0, 0, 0}
};

BS_LIBRARY_FN(bsFnRegexMatch, regexStringArgs, bsNull(), bsRegexMatchImpl(values[0], values[1]))


static BSValue bsFnRegexMatchAll(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(regexStringArgs, bsNull());
    BSRegexSubject subject;
    bsRegexSubjectInit(&subject, values[1]);
    BSValue result = bsArrayNew();
    BSRegexMatch match;
    size_t start = 0;
    while (start <= subject.length && bsRegexSearch(values[0], &subject, start, &match)) {
        bsArrayPush(result, bsRegexMatchModel(values[0], values[1], &match));
        start = (match.end > match.begin) ? match.end : match.begin + 1;
    }
    bsRegexSubjectFree(&subject);
    return result;
}


static const BSArgModel regexNewArgs[] = {
    {"pattern", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"flags", BS_ARG_STRING, BS_ARG_NULLABLE, 0, 0, 0, 0}
};

static BSValue bsFnRegexNew(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(regexNewArgs, bsNull());
    unsigned flags = 0;
    if (values[1].type == BS_STRING) {
        const char *flagText = bsStringData(values[1]);
        for (size_t ix = 0; ix < bsStringSize(values[1]); ix++) {
            if (flagText[ix] == 'i') {
                flags |= BS_REGEX_IGNORECASE;
            } else if (flagText[ix] == 'm') {
                flags |= BS_REGEX_MULTILINE;
            } else if (flagText[ix] == 's') {
                flags |= BS_REGEX_DOTALL;
            } else {
                return bsNull();
            }
        }
    }
    char error[BS_REGEX_ERROR_MAX];
    BSValue regex = bsRegexNew(bsStringData(values[0]), bsStringSize(values[0]), flags, error,
                               sizeof(error));
    if (regex.type == BS_NULL) {
        bsFunctionError(options, "%s", error);
    }
    return regex;
}


/* Append a slice of a string, by code point index, to a string builder */
static void bsSBAppendSlice(BSStringBuilder *sb, BSValue string, size_t begin, size_t end)
{
    size_t beginOffset = bsStringOffset(string, begin);
    size_t endOffset = bsStringOffset(string, end);
    bsSBAppend(sb, bsStringData(string) + beginOffset, endOffset - beginOffset);
}


/* Expand a replacement template's "$1", "$<name>", and "$$" references */
static void bsRegexExpand(BSStringBuilder *sb, BSValue regex, BSValue string, const BSRegexMatch *match,
                          const char *substr, size_t substrSize)
{
    for (size_t ix = 0; ix < substrSize; ix++) {
        if (substr[ix] != '$' || ix + 1 >= substrSize) {
            bsSBAppendChar(sb, substr[ix]);
            continue;
        }
        char next = substr[ix + 1];
        if (next == '$') {
            bsSBAppendChar(sb, '$');
            ix++;
            continue;
        }

        /* A numbered group reference - two digits when they name a group, else one; a reference to
         * no group is literal text, as JavaScript's replace reads it */
        if (next >= '0' && next <= '9') {
            size_t group = (size_t) (next - '0');
            size_t end = ix + 2;
            if (end < substrSize && substr[end] >= '0' && substr[end] <= '9' &&
                group * 10 + (size_t) (substr[end] - '0') < match->groupCount) {
                group = group * 10 + (size_t) (substr[end] - '0');
                end++;
            }
            if (group == 0 || group >= match->groupCount) {
                bsSBAppend(sb, substr + ix, end - ix);
            } else if (match->matched[group]) {
                bsSBAppendSlice(sb, string, match->groups[group].begin, match->groups[group].end);
            }
            ix = end - 1;
            continue;
        }

        /* A named group reference - literal text in a pattern with no named groups */
        if (next == '<' && bsRegexGroupsNamed(regex)) {
            size_t end = ix + 2;
            while (end < substrSize && substr[end] != '>') {
                end++;
            }
            if (end < substrSize) {
                for (size_t group = 1; group < match->groupCount; group++) {
                    BSValue name = bsRegexGroupNameValue(regex, group);
                    if (match->matched[group] && name.type == BS_STRING && bsStringSize(name) == end - ix - 2 &&
                        memcmp(bsStringData(name), substr + ix + 2, end - ix - 2) == 0) {
                        bsSBAppendSlice(sb, string, match->groups[group].begin, match->groups[group].end);
                        break;
                    }
                }
                ix = end;
                continue;
            }
        }

        bsSBAppendChar(sb, substr[ix]);
    }
}


static const BSArgModel regexReplaceArgs[] = {
    {"regex", BS_ARG_REGEX, 0, 0, 0, 0, 0},
    {"string", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"substr", BS_ARG_STRING, 0, 0, 0, 0, 0}
};

static BSValue bsFnRegexReplace(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(regexReplaceArgs, bsNull());
    BSRegexSubject subject;
    bsRegexSubjectInit(&subject, values[1]);

    BSStringBuilder sb;
    bsSBInit(&sb);
    size_t position = 0;
    BSRegexMatch match;
    while (position <= subject.length && bsRegexSearch(values[0], &subject, position, &match)) {
        bsSBAppendSlice(&sb, values[1], position, match.begin);
        bsRegexExpand(&sb, values[0], values[1], &match, bsStringData(values[2]), bsStringSize(values[2]));
        if (match.end > match.begin) {
            position = match.end;
        } else {
            if (match.begin < subject.length) {
                bsSBAppendSlice(&sb, values[1], match.begin, match.begin + 1);
            }
            position = match.begin + 1;
        }
    }
    if (position <= subject.length) {
        bsSBAppendSlice(&sb, values[1], position, subject.length);
    }
    bsRegexSubjectFree(&subject);
    return bsSBToValue(&sb);
}


static BSValue bsFnRegexSplit(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(regexStringArgs, bsNull());
    BSRegexSubject subject;
    bsRegexSubjectInit(&subject, values[1]);

    BSValue result = bsArrayNew();
    size_t position = 0;
    size_t start = 0;
    BSRegexMatch match;
    while (start <= subject.length && bsRegexSearch(values[0], &subject, start, &match)) {
        bsArrayPush(result, bsStringSlice(values[1], position, match.begin));

        /* The capture groups are part of the split result */
        for (size_t ix = 1; ix < match.groupCount; ix++) {
            if (match.matched[ix]) {
                bsArrayPush(result, bsStringSlice(values[1], match.groups[ix].begin, match.groups[ix].end));
            } else {
                bsArrayPush(result, bsNull());
            }
        }
        position = match.end;
        start = (match.end > match.begin) ? match.end : match.begin + 1;
    }
    bsArrayPush(result, bsStringSlice(values[1], position, subject.length));
    bsRegexSubjectFree(&subject);
    return result;
}


/*
 * String functions
 */


static const BSArgModel stringIndexArgs[] = {
    {"string", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"index", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_GTE, 0, 0, 0, 0}
};

static BSValue bsFnStringCharAt(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(stringIndexArgs, bsNull());
    BS_ARG_INDEX(index, values[1], bsStringLength(values[0]));
    return bsStringSlice(values[0], index, index + 1);
}


static BSValue bsFnStringCharCodeAt(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(stringIndexArgs, bsNull());
    BS_ARG_INDEX(index, values[1], bsStringLength(values[0]));
    return bsNumber(bsStringCodePoint(values[0], index));
}


static const BSArgModel stringDecodeArgs[] = {{"bytes", BS_ARG_ARRAY, 0, 0, 0, 0, 0}};

static BSValue bsFnStringDecode(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(stringDecodeArgs, bsNull());
    size_t count = bsArrayCount(values[0]);
    char *buffer = bsAlloc(count);
    for (size_t ix = 0; ix < count; ix++) {
        BSValue byte = bsArrayGet(values[0], ix);
        if (byte.type != BS_NUMBER || trunc(byte.u.number) != byte.u.number ||
            byte.u.number < 0 || byte.u.number > 255) {
            free(buffer);
            return bsNull();
        }
        buffer[ix] = (char) (unsigned char) byte.u.number;
    }
    if (bsUTF8Length(buffer, count) == SIZE_MAX) {
        free(buffer);
        return bsNull();
    }
    BSValue result = bsStringNewSize(buffer, count);
    free(buffer);
    return result;
}


static BSValue bsFnStringEncode(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(stringArgs, bsNull());
    size_t size = bsStringSize(values[0]);
    const char *text = bsStringData(values[0]);
    BSValue result = bsArrayNewCapacity(size);
    for (size_t ix = 0; ix < size; ix++) {
        bsArrayPush(result, bsNumber((unsigned char) text[ix]));
    }
    return result;
}


static const BSArgModel stringSearchArgs[] = {
    {"string", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"search", BS_ARG_STRING, 0, 0, 0, 0, 0}
};

BS_LIBRARY_FN(bsFnStringEndsWith, stringSearchArgs, bsNull(),
              bsBoolean(bsStringEndsWith(values[0], values[1])))


static BSValue bsFnStringFromCharCode(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSStringBuilder sb;
    bsSBInit(&sb);
    char utf8[4];
    for (size_t ix = 0; ix < argCount; ix++) {
        BSValue code = args[ix];
        if (code.type != BS_NUMBER || trunc(code.u.number) != code.u.number || code.u.number < 0 ||
            code.u.number > 0x10FFFF) {
            bsSBFree(&sb);
            bsArgsError(options, "charCodes", code);
            return bsNull();
        }
        bsSBAppend(&sb, utf8, bsUTF8Encode((uint32_t) code.u.number, utf8));
    }
    return bsSBToValue(&sb);
}


/* Find a substring; returns the byte offset, or SIZE_MAX if not found */
static size_t bsMemFind(const char *text, size_t size, const char *search, size_t searchSize, size_t offset)
{
    if (searchSize == 0) {
        return offset;
    }
    /* memchr finds each candidate first byte, so the scan is not a byte loop */
    const char *end = text + size;
    for (const char *at = text + offset; (at = memchr(at, search[0], (size_t) (end - at))) != NULL; at++) {
        if ((size_t) (end - at) < searchSize) {
            break;
        }
        if (memcmp(at, search, searchSize) == 0) {
            return (size_t) (at - text);
        }
    }
    return SIZE_MAX;
}


static const BSArgModel stringIndexOfArgs[] = {
    {"string", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"search", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"index", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_HAS_DEFAULT | BS_ARG_GTE, 0, 0, 0, 0}
};

static BSValue bsFnStringIndexOf(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(stringIndexOfArgs, bsNumber(-1));
    if (values[2].u.number > (double) bsStringLength(values[0])) {
        return bsArgFail(options, "index", values[2], bsNumber(-1));
    }
    size_t index = (size_t) values[2].u.number;
    size_t offset = bsMemFind(bsStringData(values[0]), bsStringSize(values[0]), bsStringData(values[1]),
                              bsStringSize(values[1]), bsStringOffset(values[0], index));
    if (offset == SIZE_MAX) {
        return bsNumber(-1);
    }
    return bsNumber((double) bsUTF8Length(bsStringData(values[0]), offset));
}


static const BSArgModel stringLastIndexOfArgs[] = {
    {"string", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"search", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"index", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_NULLABLE | BS_ARG_GTE, 0, 0, 0, 0}
};

static BSValue bsFnStringLastIndexOf(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(stringLastIndexOfArgs, bsNumber(-1));
    size_t length = bsStringLength(values[0]);
    size_t index;
    if (values[2].type == BS_NUMBER) {
        if (values[2].u.number > (double) length) {
            return bsArgFail(options, "index", values[2], bsNumber(-1));
        }
        index = (size_t) values[2].u.number;
    } else {
        index = length != 0 ? length - 1 : 0;
    }

    /* The search window ends at the search index plus the search string's length */
    size_t searchLength = bsStringLength(values[1]);
    size_t windowEnd = index + searchLength;
    size_t endOffset = bsStringOffset(values[0], windowEnd > length ? length : windowEnd);
    const char *text = bsStringData(values[0]);
    const char *search = bsStringData(values[1]);
    size_t searchSize = bsStringSize(values[1]);
    if (searchSize > endOffset) {
        return bsNumber(-1);
    }
    for (size_t ix = endOffset - searchSize + 1; ix-- > 0;) {
        if (memcmp(text + ix, search, searchSize) == 0) {
            return bsNumber((double) bsUTF8Length(text, ix));
        }
    }
    return bsNumber(-1);
}


BS_LIBRARY_FN(bsFnStringLength, stringArgs, bsNumber(0), bsNumber((double) bsStringLength(values[0])))


static BSValue bsStringCase(const BSValue *args, size_t argCount, BSOptions *options, bool upper)
{
    BS_ARGS(stringArgs, bsNull());
    size_t size = bsStringSize(values[0]);
    const char *text = bsStringData(values[0]);
    BSStringBuilder sb;
    bsSBInit(&sb);
    for (size_t ix = 0; ix < size; ix++) {
        char ch = text[ix];
        bsSBAppendChar(&sb, upper ? (char) toupper((unsigned char) ch) : (char) tolower((unsigned char) ch));
    }
    return bsSBToValue(&sb);
}


BS_RAW_FN(bsFnStringLower, bsStringCase(args, argCount, options, false))
BS_RAW_FN(bsFnStringUpper, bsStringCase(args, argCount, options, true))


BS_LIBRARY_FN(bsFnStringNewFn, valueArgs, bsNull(), bsValueString(values[0]))


static const BSArgModel stringRepeatArgs[] = {
    {"string", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"count", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_GTE, 0, 0, 4294967295, BS_ARG_LTE}
};

static BSValue bsFnStringRepeat(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(stringRepeatArgs, bsNull());
    size_t count = (size_t) values[1].u.number;
    BSStringBuilder sb;
    bsSBInit(&sb);
    for (size_t ix = 0; ix < count; ix++) {
        bsSBAppend(&sb, bsStringData(values[0]), bsStringSize(values[0]));
    }
    return bsSBToValue(&sb);
}


static const BSArgModel stringReplaceArgs[] = {
    {"string", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"substr", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"newSubstr", BS_ARG_STRING, 0, 0, 0, 0, 0}
};

static BSValue bsFnStringReplace(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(stringReplaceArgs, bsNull());
    const char *text = bsStringData(values[0]);
    size_t size = bsStringSize(values[0]);
    const char *substr = bsStringData(values[1]);
    size_t substrSize = bsStringSize(values[1]);
    BSStringBuilder sb;
    bsSBInit(&sb);
    if (substrSize == 0) {
        /* An empty search string inserts the replacement between every character */
        bsSBAppend(&sb, bsStringData(values[2]), bsStringSize(values[2]));
        size_t offset = 0;
        while (offset < size) {
            size_t codeSize;
            bsUTF8Decode(text, size, offset, &codeSize);
            bsSBAppend(&sb, text + offset, codeSize);
            bsSBAppend(&sb, bsStringData(values[2]), bsStringSize(values[2]));
            offset += codeSize;
        }
        return bsSBToValue(&sb);
    }
    size_t found = bsMemFind(text, size, substr, substrSize, 0);
    if (found == SIZE_MAX) {
        /* Nothing to replace - the string is shared */
        bsSBFree(&sb);
        return bsRetain(values[0]);
    }
    size_t position = 0;
    while (found != SIZE_MAX) {
        bsSBAppend(&sb, text + position, found - position);
        bsSBAppend(&sb, bsStringData(values[2]), bsStringSize(values[2]));
        position = found + substrSize;
        found = bsMemFind(text, size, substr, substrSize, position);
    }
    bsSBAppend(&sb, text + position, size - position);
    return bsSBToValue(&sb);
}


static const BSArgModel stringSliceArgs[] = {
    {"string", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"start", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_GTE, 0, 0, 0, 0},
    {"end", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_NULLABLE | BS_ARG_GTE, 0, 0, 0, 0}
};

static BSValue bsFnStringSlice(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(stringSliceArgs, bsNull());
    size_t start;
    size_t end;
    if (!bsArgSlice(values[1], values[2], bsStringLength(values[0]), &start, &end, options)) {
        return bsNull();
    }
    return bsStringSlice(values[0], start, end);
}


static const BSArgModel stringSplitArgs[] = {
    {"string", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"separator", BS_ARG_STRING, 0, 0, 0, 0, 0}
};

static BSValue bsFnStringSplit(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(stringSplitArgs, bsNull());
    const char *text = bsStringData(values[0]);
    size_t size = bsStringSize(values[0]);
    size_t separatorSize = bsStringSize(values[1]);
    BSValue result = bsArrayNew();

    /* An empty separator splits into individual characters */
    if (separatorSize == 0) {
        size_t offset = 0;
        while (offset < size) {
            size_t codeSize;
            bsUTF8Decode(text, size, offset, &codeSize);
            bsArrayPush(result, bsStringNewSize(text + offset, codeSize));
            offset += codeSize;
        }
        return result;
    }

    size_t position = 0;
    while (true) {
        size_t found = bsMemFind(text, size, bsStringData(values[1]), separatorSize, position);
        if (found == SIZE_MAX) {
            break;
        }
        bsArrayPush(result, bsStringNewSize(text + position, found - position));
        position = found + separatorSize;
    }
    bsArrayPush(result, bsStringNewSize(text + position, size - position));
    return result;
}


static BSValue bsFnStringSplitLines(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(stringArgs, bsNull());
    const char *text = bsStringData(values[0]);
    size_t size = bsStringSize(values[0]);
    bool ascii = bsStringLength(values[0]) == size;
    BSValue result = bsArrayNew();
    size_t position = 0;
    while (true) {
        const char *newline = memchr(text + position, '\n', size - position);
        size_t next = newline != NULL ? (size_t) (newline - text) : size;
        size_t end = next;
        if (end > position && text[end - 1] == '\r') {
            end--;
        }
        /* A line of an ASCII string is ASCII */
        bsArrayPush(result, ascii ? bsStringNewAscii(text + position, end - position) :
                    bsStringNewSize(text + position, end - position));
        if (newline == NULL) {
            return result;
        }
        position = next + 1;
    }
}


BS_LIBRARY_FN(bsFnStringStartsWith, stringSearchArgs, bsNull(),
              bsBoolean(bsStringStartsWith(values[0], values[1])))


/* Whitespace as both references trim it: ASCII's, and the Unicode spaces the two agree on */
static bool bsIsTrimSpace(uint32_t code)
{
    return code == ' ' || (code >= 0x09 && code <= 0x0D) || code == 0xA0 || code == 0x1680 ||
        (code >= 0x2000 && code <= 0x200A) || code == 0x2028 || code == 0x2029 || code == 0x202F ||
        code == 0x205F || code == 0x3000;
}

static BSValue bsFnStringTrim(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(stringArgs, bsNull());
    const char *text = bsStringData(values[0]);
    size_t size = bsStringSize(values[0]);
    size_t begin = 0;
    size_t end = size;
    size_t codeSize;
    while (begin < end && bsIsTrimSpace(bsUTF8Decode(text, end, begin, &codeSize))) {
        begin += codeSize;
    }
    while (end > begin) {
        size_t lead = end - 1;
        while (lead > begin && ((unsigned char) text[lead] & 0xC0) == 0x80) {
            lead--;
        }
        if (!bsIsTrimSpace(bsUTF8Decode(text, end, lead, &codeSize))) {
            break;
        }
        end = lead;
    }
    if (begin == 0 && end == size) {
        return bsRetain(values[0]);
    }
    return bsStringNewSize(text + begin, end - begin);
}


/*
 * System functions
 */


BS_LIBRARY_FN(bsFnSystemBoolean, valueArgs, bsNull(), bsBoolean(bsValueBoolean(values[0])))


static const BSArgModel systemCompareArgs[] = {
    {"left", BS_ARG_ANY, 0, 0, 0, 0, 0},
    {"right", BS_ARG_ANY, 0, 0, 0, 0, 0}
};

BS_LIBRARY_FN(bsFnSystemCompare, systemCompareArgs, bsNull(), bsNumber(bsValueCompare(values[0], values[1])))


/* A fetch argument is a URL string or a request model: a url string, an optional body string, and
   optional headers - an object of string values */
static bool bsFetchValid(BSValue request)
{
    if (request.type != BS_OBJECT) {
        return request.type == BS_STRING;
    }
    BSValue url = bsObjectGet(request, "url");
    BSValue body = bsObjectGet(request, "body");
    BSValue headers = bsObjectGet(request, "headers");
    if (url.type != BS_STRING || (body.type != BS_NULL && body.type != BS_STRING) ||
        (headers.type != BS_NULL && headers.type != BS_OBJECT)) {
        return false;
    }
    bool valid = true;
    if (headers.type == BS_OBJECT) {
        BSValue keys = bsObjectKeys(headers);
        for (size_t ix = 0; ix < bsArrayCount(keys) && valid; ix++) {
            valid = bsObjectGetString(headers, bsArrayGet(keys, ix)).type == BS_STRING;
        }
        bsRelease(keys);
    }
    return valid;
}


static const BSArgModel systemFetchArgs[] = {{"url", BS_ARG_ANY, 0, 0, 0, 0, 0}};

static BSValue bsFnSystemFetch(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(systemFetchArgs, bsNull());
    BSValue url = values[0];

    /* A URL string or request model, or an array of them */
    bool isArray = (url.type == BS_ARRAY);
    size_t count = isArray ? bsArrayCount(url) : 1;
    for (size_t ix = 0; ix < count; ix++) {
        BSValue item = isArray ? bsArrayGet(url, ix) : url;
        if (!bsFetchValid(item)) {
            return bsArgFail(options, "url", item, bsNull());
        }
    }
    if (count == 0) {
        return bsArrayNew();
    }

    /* Resolve each request's URL, then fetch the requests together */
    BSFetchRequest *requests = bsAlloc(count * sizeof(BSFetchRequest));
    BSValue *responses = bsAlloc(count * sizeof(BSValue));
    char **resolved = bsAlloc(count * sizeof(char *));
    for (size_t ix = 0; ix < count; ix++) {
        responses[ix] = bsNull();
        BSValue item = isArray ? bsArrayGet(url, ix) : url;
        BSValue itemUrl = item.type == BS_STRING ? item : bsObjectGet(item, "url");
        BSValue body = bsObjectGet(item, "body");
        resolved[ix] = options->urlFn != NULL ? options->urlFn(bsStringData(itemUrl), options->urlData) : NULL;
        requests[ix] = (BSFetchRequest) {
            .url = resolved[ix] != NULL ? resolved[ix] : bsStringData(itemUrl),
            .body = body.type == BS_STRING ? bsStringData(body) : NULL,
            .bodySize = body.type == BS_STRING ? bsStringSize(body) : 0,
            .headers = bsObjectGet(item, "headers")
        };
    }
    if (options->fetchFn != NULL) {
        options->fetchFn(requests, responses, count, options->fetchData);
    }

    /* The responses, logging each failure */
    BSValue result = isArray ? bsArrayNewCapacity(count) : bsNull();
    for (size_t ix = 0; ix < count; ix++) {
        if (responses[ix].type == BS_NULL && options->debug) {
            bsLog(options, "BareScript: Function \"systemFetch\" failed for resource \"%s\"", requests[ix].url);
        }
        if (isArray) {
            bsArrayPush(result, responses[ix]);
        } else {
            result = responses[ix];
        }
        free(resolved[ix]);
    }
    free(requests);
    free(responses);
    free(resolved);
    return result;
}


static const BSArgModel systemGlobalGetArgs[] = {
    {"name", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"defaultValue", BS_ARG_ANY, 0, 0, 0, 0, 0}
};

static BSValue bsFnSystemGlobalGet(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(systemGlobalGetArgs, bsNull());
    BSValue found;
    return bsRetain(bsObjectLookupString(options->globals, values[0], &found) ? found : values[1]);
}


static const BSArgModel systemGlobalSetArgs[] = {
    {"name", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"value", BS_ARG_ANY, 0, 0, 0, 0, 0}
};

BS_LIBRARY_FN(bsFnSystemGlobalSet, systemGlobalSetArgs, bsNull(), bsGlobalSetValue(options, values[0], values[1]))


static const BSArgModel systemIsArgs[] = {
    {"value1", BS_ARG_ANY, 0, 0, 0, 0, 0},
    {"value2", BS_ARG_ANY, 0, 0, 0, 0, 0}
};

BS_LIBRARY_FN(bsFnSystemIs, systemIsArgs, bsNull(), bsBoolean(bsValueIs(values[0], values[1])))


static const BSArgModel systemLogArgs[] = {{"message", BS_ARG_ANY, 0, 0, 0, 0, 0}};

static BSValue bsSystemLog(const BSValue *args, size_t argCount, BSOptions *options, bool debugOnly)
{
    BS_ARGS(systemLogArgs, bsNull());
    if (options->logFn != NULL && (options->debug || !debugOnly)) {
        BSValue text = bsValueString(values[0]);
        options->logFn(bsStringData(text), options->logData);
        bsRelease(text);
    }
    return bsNull();
}


BS_RAW_FN(bsFnSystemLog, bsSystemLog(args, argCount, options, false))
BS_RAW_FN(bsFnSystemLogDebug, bsSystemLog(args, argCount, options, true))


/* The systemPartial closure data */
typedef struct BSPartial {
    BSValue function;
    BSValue args;
} BSPartial;


static void bsPartialFree(void *data)
{
    BSPartial *partial = data;
    bsRelease(partial->function);
    bsRelease(partial->args);
    free(partial);
}


static BSValue bsPartialCall(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSPartial *partial = data;
    size_t boundCount = bsArrayCount(partial->args);
    size_t totalCount = boundCount + argCount;
    BSValue inlineArgs[16];
    BSValue *callArgs = totalCount <= 16 ? inlineArgs : bsAlloc(totalCount * sizeof(BSValue));
    for (size_t ix = 0; ix < boundCount; ix++) {
        callArgs[ix] = bsArrayGet(partial->args, ix);
    }
    for (size_t ix = 0; ix < argCount; ix++) {
        callArgs[boundCount + ix] = args[ix];
    }
    BSValue result = bsFunctionCall(partial->function, callArgs, totalCount, options);
    if (callArgs != inlineArgs) {
        free(callArgs);
    }
    return result;
}


static const BSArgModel systemPartialArgs[] = {
    {"func", BS_ARG_FUNCTION, 0, 0, 0, 0, 0},
    {"args", BS_ARG_ANY, BS_ARG_LAST_ARRAY, 0, 0, 0, 0}
};

static BSValue bsFnSystemPartial(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BS_ARGS(systemPartialArgs, bsNull());
    if (bsArrayCount(values[1]) < 1) {
        bsArgsInvalid(systemPartialArgs, 2, values, options, "args", values[1]);
        return bsNull();
    }
    BSPartial *partial = bsAlloc(sizeof(BSPartial));
    partial->function = bsRetain(values[0]);
    partial->args = bsRetain(values[1]);
    bsArgsFree(systemPartialArgs, 2, values);
    return bsFunctionNew("systemPartial", bsPartialCall, partial, bsPartialFree);
}


/* Interned by bsLibraryInit; schemaValidate compares these on every value */
static _Thread_local BSValue bsSystemTypeNames[BS_REGEX + 1];

BSValue bsSystemTypeName(BSValue value)
{
    return bsRetain(bsSystemTypeNames[value.type]);
}

BS_LIBRARY_FN(bsFnSystemType, valueArgs, bsNull(), bsSystemTypeName(values[0]))


/*
 * The library function tables
 */


/*
 * A library function: its script name, its implementation, its expression-function alias (the
 * short name an expression may call it by, if it has one), and its intrinsic id, if any
 */
typedef struct BSLibraryEntry {
    const char *name;
    BSFunctionFn fn;
    const char *alias;
    unsigned char intrinsic;
} BSLibraryEntry;


static const BSLibraryEntry bsScriptFunctionTable[] = {
    {"arrayCopy", bsFnArrayCopy, NULL, BS_INTRIN_ARRAY_COPY},
    {"arrayDelete", bsFnArrayDelete, NULL, 0},
    {"arrayExtend", bsFnArrayExtend, NULL, 0},
    {"arrayFlat", bsFnArrayFlat, NULL, 0},
    {"arrayGet", bsFnArrayGet, NULL, BS_INTRIN_ARRAY_GET},
    {"arrayIndexOf", bsFnArrayIndexOf, NULL, 0},
    {"arrayJoin", bsFnArrayJoin, NULL, 0},
    {"arrayLastIndexOf", bsFnArrayLastIndexOf, NULL, 0},
    {"arrayLength", bsFnArrayLength, NULL, BS_INTRIN_ARRAY_LENGTH},
    {"arrayNew", bsFnArrayNew, "arrayNew", BS_INTRIN_ARRAY_NEW},
    {"arrayNewSize", bsFnArrayNewSize, NULL, 0},
    {"arrayPop", bsFnArrayPop, NULL, BS_INTRIN_ARRAY_POP},
    {"arrayPush", bsFnArrayPush, NULL, BS_INTRIN_ARRAY_PUSH},
    {"arrayReverse", bsFnArrayReverse, NULL, 0},
    {"arraySet", bsFnArraySet, NULL, BS_INTRIN_ARRAY_SET},
    {"arrayShift", bsFnArrayShift, NULL, 0},
    {"arraySlice", bsFnArraySlice, NULL, 0},
    {"arraySort", bsFnArraySort, NULL, 0},
    {"barescriptEvaluateExpression", bsFnBarescriptEvaluateExpression, NULL, 0},
    {"datetimeDay", bsFnDatetimeDay, "day", 0},
    {"datetimeHour", bsFnDatetimeHour, "hour", 0},
    {"datetimeISOFormat", bsFnDatetimeISOFormat, NULL, 0},
    {"datetimeISOParse", bsFnDatetimeISOParse, NULL, 0},
    {"datetimeMillisecond", bsFnDatetimeMillisecond, "millisecond", 0},
    {"datetimeMinute", bsFnDatetimeMinute, "minute", 0},
    {"datetimeMonth", bsFnDatetimeMonth, "month", 0},
    {"datetimeNew", bsFnDatetimeNew, "date", 0},
    {"datetimeNow", bsFnDatetimeNow, "now", 0},
    {"datetimeSecond", bsFnDatetimeSecond, "second", 0},
    {"datetimeToday", bsFnDatetimeToday, "today", 0},
    {"datetimeYear", bsFnDatetimeYear, "year", 0},
    {"jsonParse", bsFnJSONParse, NULL, 0},
    {"jsonStringify", bsFnJSONStringify, NULL, 0},
    {"mathAbs", bsFnMathAbs, "abs", BS_INTRIN_MATH_ABS},
    {"mathAcos", bsFnMathAcos, "acos", 0},
    {"mathAsin", bsFnMathAsin, "asin", 0},
    {"mathAtan", bsFnMathAtan, "atan", 0},
    {"mathAtan2", bsFnMathAtan2, "atan2", 0},
    {"mathCeil", bsFnMathCeil, "ceil", BS_INTRIN_MATH_CEIL},
    {"mathCos", bsFnMathCos, "cos", 0},
    {"mathE", bsFnMathE, NULL, 0},
    {"mathFloor", bsFnMathFloor, "floor", BS_INTRIN_MATH_FLOOR},
    {"mathLn", bsFnMathLn, "ln", 0},
    {"mathLog", bsFnMathLog, "log", 0},
    {"mathMax", bsFnMathMax, "max", 0},
    {"mathMin", bsFnMathMin, "min", 0},
    {"mathPi", bsFnMathPi, "pi", 0},
    {"mathRandom", bsFnMathRandom, "rand", 0},
    {"mathRound", bsFnMathRound, "round", 0},
    {"mathSign", bsFnMathSign, "sign", BS_INTRIN_MATH_SIGN},
    {"mathSin", bsFnMathSin, "sin", 0},
    {"mathSqrt", bsFnMathSqrt, "sqrt", BS_INTRIN_MATH_SQRT},
    {"mathTan", bsFnMathTan, "tan", 0},
    {"numberParseFloat", bsFnNumberParseFloat, "parseFloat", 0},
    {"numberParseInt", bsFnNumberParseInt, "parseInt", BS_INTRIN_NUMBER_PARSE_INT},
    {"numberToFixed", bsFnNumberToFixed, "fixed", 0},
    {"numberToString", bsFnNumberToString, NULL, 0},
    {"objectAssign", bsFnObjectAssign, NULL, 0},
    {"objectCopy", bsFnObjectCopy, NULL, BS_INTRIN_OBJECT_COPY},
    {"objectDelete", bsFnObjectDelete, NULL, BS_INTRIN_OBJECT_DELETE},
    {"objectGet", bsFnObjectGet, NULL, BS_INTRIN_OBJECT_GET},
    {"objectHas", bsFnObjectHas, NULL, BS_INTRIN_OBJECT_HAS},
    {"objectKeys", bsFnObjectKeys, NULL, BS_INTRIN_OBJECT_KEYS},
    {"objectNew", bsFnObjectNew, "objectNew", BS_INTRIN_OBJECT_NEW},
    {"objectSet", bsFnObjectSet, NULL, BS_INTRIN_OBJECT_SET},
    {"regexEscape", bsFnRegexEscape, NULL, 0},
    {"regexMatch", bsFnRegexMatch, NULL, BS_INTRIN_REGEX_MATCH},
    {"regexMatchAll", bsFnRegexMatchAll, NULL, 0},
    {"regexNew", bsFnRegexNew, NULL, 0},
    {"regexReplace", bsFnRegexReplace, NULL, 0},
    {"regexSplit", bsFnRegexSplit, NULL, 0},
    {"stringCharAt", bsFnStringCharAt, NULL, 0},
    {"stringCharCodeAt", bsFnStringCharCodeAt, "charCodeAt", BS_INTRIN_STRING_CHAR_CODE_AT},
    {"stringDecode", bsFnStringDecode, NULL, 0},
    {"stringEncode", bsFnStringEncode, NULL, 0},
    {"stringEndsWith", bsFnStringEndsWith, "endsWith", BS_INTRIN_STRING_ENDS_WITH},
    {"stringFromCharCode", bsFnStringFromCharCode, "fromCharCode", 0},
    {"stringIndexOf", bsFnStringIndexOf, "indexOf", 0},
    {"stringLastIndexOf", bsFnStringLastIndexOf, "lastIndexOf", 0},
    {"stringLength", bsFnStringLength, "len", BS_INTRIN_STRING_LENGTH},
    {"stringLower", bsFnStringLower, "lower", 0},
    {"stringNew", bsFnStringNewFn, "text", 0},
    {"stringRepeat", bsFnStringRepeat, "rept", 0},
    {"stringReplace", bsFnStringReplace, "replace", 0},
    {"stringSlice", bsFnStringSlice, "slice", BS_INTRIN_STRING_SLICE},
    {"stringSplit", bsFnStringSplit, NULL, 0},
    {"stringSplitLines", bsFnStringSplitLines, NULL, 0},
    {"stringStartsWith", bsFnStringStartsWith, "startsWith", BS_INTRIN_STRING_STARTS_WITH},
    {"stringTrim", bsFnStringTrim, "trim", 0},
    {"stringUpper", bsFnStringUpper, "upper", 0},
    {"systemBoolean", bsFnSystemBoolean, NULL, BS_INTRIN_SYSTEM_BOOLEAN},
    {"systemCompare", bsFnSystemCompare, NULL, 0},
    {"systemFetch", bsFnSystemFetch, NULL, 0},
    {"systemGlobalGet", bsFnSystemGlobalGet, NULL, 0},
    {"systemGlobalSet", bsFnSystemGlobalSet, NULL, BS_INTRIN_SYSTEM_GLOBAL_SET},
    {"systemIs", bsFnSystemIs, NULL, 0},
    {"systemLog", bsFnSystemLog, NULL, 0},
    {"systemLogDebug", bsFnSystemLogDebug, NULL, 0},
    {"systemPartial", bsFnSystemPartial, NULL, 0},
    {"systemType", bsFnSystemType, NULL, BS_INTRIN_SYSTEM_TYPE}
};

#define BS_SCRIPT_FUNCTION_COUNT (sizeof(bsScriptFunctionTable) / sizeof(bsScriptFunctionTable[0]))


/*
 * The library function value cache
 *
 * Library function values are created once per thread and shared by every globals object on it,
 * so a library function has a stable identity - which systemIs relies on, and which lets an
 * override be detected by pointer inequality.
 */
static _Thread_local BSValue bsScriptFunctionValues = {BS_NULL, {0}};
static _Thread_local BSValue bsExpressionFunctionValues = {BS_NULL, {0}};


static void bsLibraryInit(void)
{
    if (bsScriptFunctionValues.type == BS_OBJECT) {
        return;
    }
    bsScriptFunctionValues = bsObjectNew();
    bsExpressionFunctionValues = bsObjectNew();
    for (size_t ix = 0; ix < BS_SCRIPT_FUNCTION_COUNT; ix++) {
        const BSLibraryEntry *entry = &bsScriptFunctionTable[ix];
        BSValue function = bsFunctionNew(entry->name, entry->fn, NULL, NULL);
        function.u.function->intrinsic = entry->intrinsic;
        if (entry->alias != NULL) {
            bsObjectSet(bsExpressionFunctionValues, entry->alias, bsRetain(function));
        }
        bsObjectSet(bsScriptFunctionValues, entry->name, function);
    }

    /* The strings the regex match model and systemType intern once */
    bsMatchKeys.index = bsStringIntern("index", 5);
    bsMatchKeys.input = bsStringIntern("input", 5);
    bsMatchKeys.groups = bsStringIntern("groups", 6);
    bsMatchKeys.empty = bsStringIntern("", 0);
    bsMatchKeyGroupGrow(10);
    for (int ix = 0; ix <= (int) BS_REGEX; ix++) {
        bsSystemTypeNames[ix] = bsStringIntern(bsTypeNames[ix], strlen(bsTypeNames[ix]));
    }
}


void bsLibraryGlobals(BSValue globals)
{
    bsLibraryInit();
    for (size_t ix = 0; ix < BS_SCRIPT_FUNCTION_COUNT; ix++) {
        const char *name = bsScriptFunctionTable[ix].name;
        if (!bsObjectHas(globals, name)) {
            bsObjectSet(globals, name, bsRetain(bsObjectGet(bsScriptFunctionValues, name)));
        }
    }
}


BSValue bsLibraryExpressionFunction(BSValue name)
{
    bsLibraryInit();
    return bsObjectGetString(bsExpressionFunctionValues, name);
}


BSValue bsLibraryScriptFunction(const char *name)
{
    bsLibraryInit();
    return bsObjectGet(bsScriptFunctionValues, name);
}


void bsLibraryCleanup(void)
{
    bsFetchCleanup();
    if (bsScriptFunctionValues.type != BS_OBJECT) {
        return;
    }
    bsRelease(bsExpressionFunctionValues);
    bsRelease(bsScriptFunctionValues);
    bsExpressionFunctionValues = bsNull();
    bsScriptFunctionValues = bsNull();
    bsRelease(bsMatchKeys.index);
    bsRelease(bsMatchKeys.input);
    bsRelease(bsMatchKeys.groups);
    bsRelease(bsMatchKeys.empty);
    for (size_t ix = 0; ix < bsMatchKeys.groupCount; ix++) {
        bsRelease(bsMatchKeys.group[ix]);
    }
    free(bsMatchKeys.group);
    bsMatchKeys.group = NULL;
    bsMatchKeys.groupCount = 0;
    for (int ix = 0; ix <= (int) BS_REGEX; ix++) {
        bsRelease(bsSystemTypeNames[ix]);
    }
}
