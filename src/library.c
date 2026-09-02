/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The BareScript library
 *
 * Every library function has the same shape: validate the arguments against a static argument
 * model, do the work, release the validated arguments. bsArgsValidate applies the same coercion
 * and range rules as the reference implementations' value_args_validate, including the documented
 * per-function error return values.
 */

#include <ctype.h>
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


static bool bsArgTypeMatch(BSArgType type, BSValue value)
{
    switch (type) {
    case BS_ARG_NUMBER:
        return value.type == BS_NUMBER;
    case BS_ARG_STRING:
        return value.type == BS_STRING;
    case BS_ARG_ARRAY:
        return value.type == BS_ARRAY;
    case BS_ARG_OBJECT:
        return value.type == BS_OBJECT;
    case BS_ARG_DATETIME:
        return value.type == BS_DATETIME;
    case BS_ARG_REGEX:
        return value.type == BS_REGEX;
    default:
        return value.type == BS_FUNCTION;
    }
}


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


bool bsArgsValidate(const BSArgModel *argModel, size_t argModelCount, const BSValue *args, size_t argCount,
                    BSValue *values, BSOptions *options, const char *functionName)
{
    /* LAST_ARRAY slots are released on failure; every slot must be a valid value first */
    for (size_t ix = 0; ix < argModelCount; ix++) {
        values[ix] = bsNull();
    }
    for (size_t ix = 0; ix < argModelCount; ix++) {
        const BSArgModel *model = &argModel[ix];

        /* The last-argument array collects every remaining argument */
        if ((model->flags & BS_ARG_LAST_ARRAY) != 0) {
            size_t restCount = argCount > ix ? argCount - ix : 0;
            BSValue rest = bsArrayNewCapacity(restCount);
            for (size_t ixRest = ix; ixRest < argCount; ixRest++) {
                bsArrayPush(rest, bsRetain(args[ixRest]));
            }
            values[ix] = rest;
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
            bsArgsError(options, model->name, bsNull());
            bsArgsFree(argModel, argModelCount, values);
            return false;
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
                bsArgsError(options, model->name, value);
                bsArgsFree(argModel, argModelCount, values);
                return false;
            }
            continue;
        }

        if (!bsArgTypeMatch(model->type, value)) {
            bsArgsError(options, model->name, value);
            bsArgsFree(argModel, argModelCount, values);
            return false;
        }
        if (model->type == BS_ARG_NUMBER) {
            double number = value.u.number;
            if (((model->flags & BS_ARG_INTEGER) != 0 && (!isfinite(number) || trunc(number) != number)) ||
                !bsArgLimit(number, model->flags, model->limit) ||
                !bsArgLimit(number, model->flags2, model->limit2)) {
                bsArgsError(options, model->name, value);
                bsArgsFree(argModel, argModelCount, values);
                return false;
            }
        }
        values[ix] = value;
    }

    /* Extra arguments */
    if (argCount > argModelCount) {
        bsArgsError(options, NULL, bsNumber((double) argCount));
        bsArgsFree(argModel, argModelCount, values);
        return false;
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


/* Report an argument error from a function that validates a value itself */
static BSValue bsArgFail(BSOptions *options, const char *argName, BSValue argValue, BSValue errorValue)
{
    bsArgsError(options, argName, argValue);
    return errorValue;
}


/*
 * Array functions
 */


static const BSArgModel arrayCopyArgs[] = {{"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0}};

static BSValue bsFnArrayCopy(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(arrayCopyArgs, 1, args, argCount, values, options, "arrayCopy")) {
        return bsNull();
    }
    return bsArrayCopy(values[0]);
}


static const BSArgModel arrayDeleteArgs[] = {
    {"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0},
    {"index", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_GTE, 0, 0, 0, 0}
};

static BSValue bsFnArrayDelete(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(arrayDeleteArgs, 2, args, argCount, values, options, "arrayDelete")) {
        return bsNull();
    }
    size_t index = (size_t) values[1].u.number;
    if (index >= bsArrayCount(values[0])) {
        return bsArgFail(options, "index", values[1], bsNull());
    }
    bsArrayDelete(values[0], index);
    return bsNull();
}


static const BSArgModel arrayExtendArgs[] = {
    {"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0},
    {"array2", BS_ARG_ARRAY, 0, 0, 0, 0, 0}
};

static BSValue bsFnArrayExtend(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(arrayExtendArgs, 2, args, argCount, values, options, "arrayExtend")) {
        return bsNull();
    }
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
    BSValue values[2];
    if (!bsArgsValidate(arrayFlatArgs, 2, args, argCount, values, options, "arrayFlat")) {
        return bsNull();
    }
    BSValue result = bsArrayNew();
    double depth = values[1].u.number;
    int maxDepth = depth < 0 ? 0 : (depth > 1000000 ? 1000000 : (int) depth);
    bsArrayFlatHelper(result, values[0], 0, maxDepth);
    return result;
}


static const BSArgModel arrayGetArgs[] = {
    {"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0},
    {"index", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_GTE, 0, 0, 0, 0}
};

static BSValue bsFnArrayGet(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(arrayGetArgs, 2, args, argCount, values, options, "arrayGet")) {
        return bsNull();
    }
    size_t index = (size_t) values[1].u.number;
    if (index >= bsArrayCount(values[0])) {
        return bsArgFail(options, "index", values[1], bsNull());
    }
    return bsRetain(bsArrayGet(values[0], index));
}


static const BSArgModel arrayIndexOfArgs[] = {
    {"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0},
    {"value", BS_ARG_ANY, 0, 0, 0, 0, 0},
    {"index", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_HAS_DEFAULT | BS_ARG_GTE, 0, 0, 0, 0}
};

static BSValue bsFnArrayIndexOf(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[3];
    if (!bsArgsValidate(arrayIndexOfArgs, 3, args, argCount, values, options, "arrayIndexOf")) {
        return bsNumber(-1);
    }
    size_t count = bsArrayCount(values[0]);
    size_t index = (size_t) values[2].u.number;
    bool isFunction = (values[1].type == BS_FUNCTION);
    for (size_t ix = index; ix < count; ix++) {
        BSValue item = bsArrayGet(values[0], ix);
        bool matched;
        if (isFunction) {
            BSValue result = bsFunctionCall(values[1], &item, 1, options);
            matched = bsValueBoolean(result);
            bsRelease(result);
            if (options->error.type == BS_STRING) {
                return bsNumber(-1);
            }
        } else {
            matched = (bsValueCompare(item, values[1]) == 0);
        }
        if (matched) {
            return bsNumber((double) ix);
        }
    }
    return bsNumber(-1);
}


static const BSArgModel arrayJoinArgs[] = {
    {"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0},
    {"separator", BS_ARG_STRING, 0, 0, 0, 0, 0}
};

static BSValue bsFnArrayJoin(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(arrayJoinArgs, 2, args, argCount, values, options, "arrayJoin")) {
        return bsNull();
    }
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
    BSValue values[3];
    if (!bsArgsValidate(arrayLastIndexOfArgs, 3, args, argCount, values, options, "arrayLastIndexOf")) {
        return bsNumber(-1);
    }
    size_t count = bsArrayCount(values[0]);
    if (count == 0) {
        return bsNumber(-1);
    }
    size_t index = (values[2].type == BS_NUMBER && (size_t) values[2].u.number < count) ?
        (size_t) values[2].u.number : count - 1;
    bool isFunction = (values[1].type == BS_FUNCTION);
    for (size_t ix = index + 1; ix-- > 0;) {
        BSValue item = bsArrayGet(values[0], ix);
        bool matched;
        if (isFunction) {
            BSValue result = bsFunctionCall(values[1], &item, 1, options);
            matched = bsValueBoolean(result);
            bsRelease(result);
            if (options->error.type == BS_STRING) {
                return bsNumber(-1);
            }
        } else {
            matched = (bsValueCompare(item, values[1]) == 0);
        }
        if (matched) {
            return bsNumber((double) ix);
        }
    }
    return bsNumber(-1);
}


static const BSArgModel arrayLengthArgs[] = {{"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0}};

static BSValue bsFnArrayLength(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(arrayLengthArgs, 1, args, argCount, values, options, "arrayLength")) {
        return bsNumber(0);
    }
    return bsNumber((double) bsArrayCount(values[0]));
}


static BSValue bsFnArrayNew(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue result = bsArrayNewCapacity(argCount);
    for (size_t ix = 0; ix < argCount; ix++) {
        bsArrayPush(result, bsRetain(args[ix]));
    }
    return result;
}


static const BSArgModel arrayNewSizeArgs[] = {
    {"size", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_HAS_DEFAULT | BS_ARG_GTE, 0, 0, 0, 0},
    {"value", BS_ARG_ANY, BS_ARG_HAS_DEFAULT, 0, 0, 0, 0}
};

static BSValue bsFnArrayNewSize(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(arrayNewSizeArgs, 2, args, argCount, values, options, "arrayNewSize")) {
        return bsNull();
    }
    size_t size = (size_t) values[0].u.number;
    BSValue result = bsArrayNewCapacity(size);
    for (size_t ix = 0; ix < size; ix++) {
        bsArrayPush(result, bsRetain(values[1]));
    }
    return result;
}


static const BSArgModel arrayPopArgs[] = {{"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0}};

static BSValue bsFnArrayPop(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(arrayPopArgs, 1, args, argCount, values, options, "arrayPop")) {
        return bsNull();
    }
    size_t count = bsArrayCount(values[0]);
    if (count == 0) {
        return bsArgFail(options, "array", values[0], bsNull());
    }
    BSValue result = bsRetain(bsArrayGet(values[0], count - 1));
    bsArrayDelete(values[0], count - 1);
    return result;
}


static const BSArgModel arrayPushArgs[] = {
    {"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0},
    {"values", BS_ARG_ANY, BS_ARG_LAST_ARRAY, 0, 0, 0, 0}
};

static BSValue bsFnArrayPush(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(arrayPushArgs, 2, args, argCount, values, options, "arrayPush")) {
        return bsNull();
    }
    size_t count = bsArrayCount(values[1]);
    for (size_t ix = 0; ix < count; ix++) {
        bsArrayPush(values[0], bsRetain(bsArrayGet(values[1], ix)));
    }
    bsArgsFree(arrayPushArgs, 2, values);
    return bsRetain(values[0]);
}


static const BSArgModel arrayReverseArgs[] = {{"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0}};

static BSValue bsFnArrayReverse(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(arrayReverseArgs, 1, args, argCount, values, options, "arrayReverse")) {
        return bsNull();
    }
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
    BSValue values[3];
    if (!bsArgsValidate(arraySetArgs, 3, args, argCount, values, options, "arraySet")) {
        return bsNull();
    }
    size_t index = (size_t) values[1].u.number;
    if (index >= bsArrayCount(values[0])) {
        return bsArgFail(options, "index", values[1], bsNull());
    }
    bsArraySet(values[0], index, bsRetain(values[2]));
    return bsRetain(values[2]);
}


static const BSArgModel arrayShiftArgs[] = {{"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0}};

static BSValue bsFnArrayShift(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(arrayShiftArgs, 1, args, argCount, values, options, "arrayShift")) {
        return bsNull();
    }
    if (bsArrayCount(values[0]) == 0) {
        return bsArgFail(options, "array", values[0], bsNull());
    }
    BSValue result = bsRetain(bsArrayGet(values[0], 0));
    bsArrayDelete(values[0], 0);
    return result;
}


static const BSArgModel arraySliceArgs[] = {
    {"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0},
    {"start", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_HAS_DEFAULT | BS_ARG_GTE, 0, 0, 0, 0},
    {"end", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_NULLABLE | BS_ARG_GTE, 0, 0, 0, 0}
};

static BSValue bsFnArraySlice(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[3];
    if (!bsArgsValidate(arraySliceArgs, 3, args, argCount, values, options, "arraySlice")) {
        return bsNull();
    }
    size_t count = bsArrayCount(values[0]);
    size_t start = (size_t) values[1].u.number;
    size_t end = values[2].type == BS_NUMBER ? (size_t) values[2].u.number : count;
    if (start > count) {
        return bsArgFail(options, "start", values[1], bsNull());
    }
    if (end > count) {
        return bsArgFail(options, "end", values[2], bsNull());
    }
    BSValue result = bsArrayNewCapacity(end > start ? end - start : 0);
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
    BSValue values[2];
    if (!bsArgsValidate(arraySortArgs, 2, args, argCount, values, options, "arraySort")) {
        return bsNull();
    }
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

static BSValue bsFnBarescriptEvaluateExpression(const BSValue *args, size_t argCount, BSOptions *options,
                                                void *data)
{
    BSValue values[3];
    if (!bsArgsValidate(barescriptEvaluateExpressionArgs, 3, args, argCount, values, options,
                        "barescriptEvaluateExpression")) {
        return bsNull();
    }
    return bsEvaluateExpressionModel(values[0], options, values[1], values[2].u.boolean);
}


/*
 * Datetime functions
 */


static const BSArgModel datetimeArgs[] = {{"datetime", BS_ARG_DATETIME, 0, 0, 0, 0, 0}};

#define BS_DATETIME_PART(fnName, name, member) \
    static BSValue fnName(const BSValue *args, size_t argCount, BSOptions *options, void *data) \
    { \
        BSValue values[1]; \
        if (!bsArgsValidate(datetimeArgs, 1, args, argCount, values, options, name)) { \
            return bsNull(); \
        } \
        BSDatetimeParts parts; \
        bsDatetimeParts(values[0].u.datetime, &parts); \
        return bsNumber(parts.member); \
    }

BS_DATETIME_PART(bsFnDatetimeDay, "datetimeDay", day)
BS_DATETIME_PART(bsFnDatetimeHour, "datetimeHour", hour)
BS_DATETIME_PART(bsFnDatetimeMillisecond, "datetimeMillisecond", millisecond)
BS_DATETIME_PART(bsFnDatetimeMinute, "datetimeMinute", minute)
BS_DATETIME_PART(bsFnDatetimeMonth, "datetimeMonth", month)
BS_DATETIME_PART(bsFnDatetimeSecond, "datetimeSecond", second)
BS_DATETIME_PART(bsFnDatetimeYear, "datetimeYear", year)


static const BSArgModel datetimeISOFormatArgs[] = {
    {"datetime", BS_ARG_DATETIME, 0, 0, 0, 0, 0},
    {"isDate", BS_ARG_BOOLEAN, BS_ARG_HAS_DEFAULT, 0, 0, 0, 0}
};

static BSValue bsFnDatetimeISOFormat(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(datetimeISOFormatArgs, 2, args, argCount, values, options, "datetimeISOFormat")) {
        return bsNull();
    }
    if (!values[1].u.boolean) {
        return bsValueString(values[0]);
    }
    BSDatetimeParts parts;
    bsDatetimeParts(values[0].u.datetime, &parts);
    return bsStringNewFormat("%04d-%02d-%02d", parts.year, parts.month, parts.day);
}


static const BSArgModel datetimeISOParseArgs[] = {{"string", BS_ARG_STRING, 0, 0, 0, 0, 0}};

static BSValue bsFnDatetimeISOParse(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(datetimeISOParseArgs, 1, args, argCount, values, options, "datetimeISOParse")) {
        return bsNull();
    }
    int64_t milliseconds;
    if (!bsDatetimeParse(bsStringData(values[0]), bsStringSize(values[0]), &milliseconds)) {
        return bsNull();
    }
    return bsDatetime(milliseconds);
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

static BSValue bsFnDatetimeNew(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[7];
    if (!bsArgsValidate(datetimeNewArgs, 7, args, argCount, values, options, "datetimeNew")) {
        return bsNull();
    }
    return bsDatetime(bsDatetimeFromParts(values[0].u.number, values[1].u.number, values[2].u.number,
                                          values[3].u.number, values[4].u.number, values[5].u.number,
                                          values[6].u.number));
}


static BSValue bsFnDatetimeNow(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    return bsDatetime(bsDatetimeNow());
}


static BSValue bsFnDatetimeToday(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    return bsDatetime(bsDatetimeToday());
}


/*
 * JSON functions
 */


static const BSArgModel jsonParseArgs[] = {{"string", BS_ARG_STRING, 0, 0, 0, 0, 0}};

static BSValue bsFnJSONParse(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(jsonParseArgs, 1, args, argCount, values, options, "jsonParse")) {
        return bsNull();
    }
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
    {"indent", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_NULLABLE | BS_ARG_GTE, 0, 1, 0, 0}
};

static BSValue bsFnJSONStringify(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(jsonStringifyArgs, 2, args, argCount, values, options, "jsonStringify")) {
        return bsNull();
    }
    return bsJSONEncode(values[0], values[1].type == BS_NUMBER ? (int) values[1].u.number : 0);
}


/*
 * Math functions
 */


static const BSArgModel mathXArgs[] = {{"x", BS_ARG_NUMBER, 0, 0, 0, 0, 0}};

#define BS_MATH_FN(fnName, name, expression) \
    static BSValue fnName(const BSValue *args, size_t argCount, BSOptions *options, void *data) \
    { \
        BSValue values[1]; \
        if (!bsArgsValidate(mathXArgs, 1, args, argCount, values, options, name)) { \
            return bsNull(); \
        } \
        double x = values[0].u.number; \
        return bsNumber(expression); \
    }

BS_MATH_FN(bsFnMathAbs, "mathAbs", fabs(x))
BS_MATH_FN(bsFnMathAcos, "mathAcos", acos(x))
BS_MATH_FN(bsFnMathAsin, "mathAsin", asin(x))
BS_MATH_FN(bsFnMathAtan, "mathAtan", atan(x))
BS_MATH_FN(bsFnMathCeil, "mathCeil", ceil(x))
BS_MATH_FN(bsFnMathCos, "mathCos", cos(x))
BS_MATH_FN(bsFnMathFloor, "mathFloor", floor(x))
BS_MATH_FN(bsFnMathSign, "mathSign", x < 0 ? -1 : (x == 0 ? 0 : 1))
BS_MATH_FN(bsFnMathSin, "mathSin", sin(x))
BS_MATH_FN(bsFnMathTan, "mathTan", tan(x))


static const BSArgModel mathAtan2Args[] = {
    {"y", BS_ARG_NUMBER, 0, 0, 0, 0, 0},
    {"x", BS_ARG_NUMBER, 0, 0, 0, 0, 0}
};

static BSValue bsFnMathAtan2(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(mathAtan2Args, 2, args, argCount, values, options, "mathAtan2")) {
        return bsNull();
    }
    return bsNumber(atan2(values[0].u.number, values[1].u.number));
}


static BSValue bsFnMathE(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    return bsNumber(2.718281828459045);
}


static BSValue bsFnMathPi(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    return bsNumber(3.141592653589793);
}


static const BSArgModel mathLnArgs[] = {{"x", BS_ARG_NUMBER, BS_ARG_GT, 0, 0, 0, 0}};

static BSValue bsFnMathLn(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(mathLnArgs, 1, args, argCount, values, options, "mathLn")) {
        return bsNull();
    }
    return bsNumber(log(values[0].u.number));
}


static const BSArgModel mathLogArgs[] = {
    {"x", BS_ARG_NUMBER, BS_ARG_GT, 0, 0, 0, 0},
    {"base", BS_ARG_NUMBER, BS_ARG_HAS_DEFAULT | BS_ARG_GT, 10, 0, 0, 0}
};

static BSValue bsFnMathLog(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(mathLogArgs, 2, args, argCount, values, options, "mathLog")) {
        return bsNull();
    }
    if (values[1].u.number == 1) {
        return bsArgFail(options, "base", values[1], bsNull());
    }
    return bsNumber(log(values[0].u.number) / log(values[1].u.number));
}


static BSValue bsFnMathMax(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue result = bsNull();
    for (size_t ix = 0; ix < argCount; ix++) {
        if (ix == 0 || bsValueCompare(args[ix], result) > 0) {
            result = args[ix];
        }
    }
    return bsRetain(result);
}


static BSValue bsFnMathMin(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue result = bsNull();
    for (size_t ix = 0; ix < argCount; ix++) {
        if (ix == 0 || bsValueCompare(args[ix], result) < 0) {
            result = args[ix];
        }
    }
    return bsRetain(result);
}


/* The random number generator - a deterministic xorshift, seeded from the clock at first use */
static uint64_t bsRandomState;

static BSValue bsFnMathRandom(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    if (bsRandomState == 0) {
        bsRandomState = (uint64_t) bsDatetimeNow() | 1u;
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

static BSValue bsFnMathRound(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(mathRoundArgs, 2, args, argCount, values, options, "mathRound")) {
        return bsNull();
    }
    return bsNumber(bsNumberRound(values[0].u.number, (int) values[1].u.number));
}


static const BSArgModel mathSqrtArgs[] = {{"x", BS_ARG_NUMBER, BS_ARG_GTE, 0, 0, 0, 0}};

static BSValue bsFnMathSqrt(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(mathSqrtArgs, 1, args, argCount, values, options, "mathSqrt")) {
        return bsNull();
    }
    return bsNumber(sqrt(values[0].u.number));
}


/*
 * Number functions
 */


static const BSArgModel numberParseFloatArgs[] = {{"string", BS_ARG_STRING, 0, 0, 0, 0, 0}};

static BSValue bsFnNumberParseFloat(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(numberParseFloatArgs, 1, args, argCount, values, options, "numberParseFloat")) {
        return bsNull();
    }
    double number;
    if (!bsNumberParse(bsStringData(values[0]), bsStringSize(values[0]), &number)) {
        return bsNull();
    }
    return bsNumber(number);
}


static const BSArgModel numberParseIntArgs[] = {
    {"string", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"radix", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_HAS_DEFAULT | BS_ARG_GTE, 10, 2, 36, BS_ARG_LTE}
};

static BSValue bsFnNumberParseInt(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(numberParseIntArgs, 2, args, argCount, values, options, "numberParseInt")) {
        return bsNull();
    }
    double number;
    if (!bsIntegerParse(bsStringData(values[0]), bsStringSize(values[0]), (int) values[1].u.number, &number)) {
        return bsNull();
    }
    return bsNumber(number);
}


static const BSArgModel numberToFixedArgs[] = {
    {"x", BS_ARG_NUMBER, 0, 0, 0, 0, 0},
    {"digits", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_HAS_DEFAULT | BS_ARG_GTE, 2, 0, 0, 0},
    {"trim", BS_ARG_BOOLEAN, BS_ARG_HAS_DEFAULT, 0, 0, 0, 0}
};

static BSValue bsFnNumberToFixed(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[3];
    if (!bsArgsValidate(numberToFixedArgs, 3, args, argCount, values, options, "numberToFixed")) {
        return bsNull();
    }
    int digits = (int) values[1].u.number;
    if (digits > 100) {
        return bsArgFail(options, "digits", values[1], bsNull());
    }
    BSValue result = bsStringNewFormat("%.*f", digits, bsNumberRound(values[0].u.number, digits));
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

static BSValue bsFnNumberToString(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(numberToStringArgs, 2, args, argCount, values, options, "numberToString")) {
        return bsNull();
    }
    static const char digitChars[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    double x = values[0].u.number;
    int radix = (int) values[1].u.number;
    if (x == 0) {
        return bsStringNew("0");
    }
    char buffer[80];
    size_t size = 0;
    while (x >= 1 && size < sizeof(buffer)) {
        double quotient = floor(x / radix);
        int digit = (int) (x - quotient * radix);
        buffer[size++] = digitChars[digit];
        x = quotient;
    }
    for (size_t ix = 0; ix < size / 2; ix++) {
        char swap = buffer[ix];
        buffer[ix] = buffer[size - 1 - ix];
        buffer[size - 1 - ix] = swap;
    }
    return bsStringNewSize(buffer, size);
}


/*
 * Object functions
 */


static bool bsObjectAssignIter(BSValue key, BSValue item, void *data)
{
    bsObjectSetString(*((BSValue *) data), key, bsRetain(item));
    return true;
}


static const BSArgModel objectAssignArgs[] = {
    {"object", BS_ARG_OBJECT, 0, 0, 0, 0, 0},
    {"object2", BS_ARG_OBJECT, 0, 0, 0, 0, 0}
};

static BSValue bsFnObjectAssign(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(objectAssignArgs, 2, args, argCount, values, options, "objectAssign")) {
        return bsNull();
    }
    bsObjectIter(values[1], bsObjectAssignIter, &values[0]);
    return bsRetain(values[0]);
}


static const BSArgModel objectCopyArgs[] = {{"object", BS_ARG_OBJECT, 0, 0, 0, 0, 0}};

static BSValue bsFnObjectCopy(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(objectCopyArgs, 1, args, argCount, values, options, "objectCopy")) {
        return bsNull();
    }
    return bsObjectCopy(values[0]);
}


static const BSArgModel objectDeleteArgs[] = {
    {"object", BS_ARG_OBJECT, 0, 0, 0, 0, 0},
    {"key", BS_ARG_STRING, 0, 0, 0, 0, 0}
};

static BSValue bsFnObjectDelete(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(objectDeleteArgs, 2, args, argCount, values, options, "objectDelete")) {
        return bsNull();
    }
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
    BSValue defaultValue = argCount >= 3 ? args[2] : bsNull();
    BSValue values[3];
    if (!bsArgsValidate(objectGetArgs, 3, args, argCount, values, options, "objectGet")) {
        return bsRetain(defaultValue);
    }
    BSValue found;
    if (!bsObjectLookup(values[0], bsStringData(values[1]), bsStringSize(values[1]), &found)) {
        return bsRetain(values[2]);
    }
    return bsRetain(found);
}


static const BSArgModel objectHasArgs[] = {
    {"object", BS_ARG_OBJECT, 0, 0, 0, 0, 0},
    {"key", BS_ARG_STRING, 0, 0, 0, 0, 0}
};

static BSValue bsFnObjectHas(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(objectHasArgs, 2, args, argCount, values, options, "objectHas")) {
        return bsBoolean(false);
    }
    return bsBoolean(bsObjectHasString(values[0], values[1]));
}


static const BSArgModel objectKeysArgs[] = {{"object", BS_ARG_OBJECT, 0, 0, 0, 0, 0}};

static BSValue bsFnObjectKeys(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(objectKeysArgs, 1, args, argCount, values, options, "objectKeys")) {
        return bsNull();
    }
    return bsObjectKeys(values[0]);
}


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
    BSValue values[3];
    if (!bsArgsValidate(objectSetArgs, 3, args, argCount, values, options, "objectSet")) {
        return bsNull();
    }
    bsObjectSetString(values[0], values[1], bsRetain(values[2]));
    return bsRetain(values[2]);
}


/*
 * Regex functions
 */


static const BSArgModel regexEscapeArgs[] = {{"string", BS_ARG_STRING, 0, 0, 0, 0, 0}};

static BSValue bsFnRegexEscape(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(regexEscapeArgs, 1, args, argCount, values, options, "regexEscape")) {
        return bsNull();
    }
    return bsRegexEscape(values[0]);
}


static BSValue bsMatchKeyIndex;
static BSValue bsMatchKeyInput;
static BSValue bsMatchKeyGroups;
static BSValue bsMatchKeyDigit[10];
static BSValue bsMatchEmpty;

static void bsMatchKeysInit(void)
{
    if (bsMatchKeyIndex.type == BS_STRING) {
        return;
    }
    bsMatchKeyIndex = bsStringIntern("index", 5);
    bsMatchKeyInput = bsStringIntern("input", 5);
    bsMatchKeyGroups = bsStringIntern("groups", 6);
    bsMatchEmpty = bsStringIntern("", 0);
    for (int ix = 0; ix < 10; ix++) {
        char digit = (char) ('0' + ix);
        bsMatchKeyDigit[ix] = bsStringIntern(&digit, 1);
    }
}


/* Create a match model object - the "index", "input", and "groups" members */
static BSValue bsRegexMatchModel(BSValue regex, BSValue string, const BSRegexSubject *subject,
                                 const BSRegexMatch *match)
{
    bsMatchKeysInit();
    BSValue groups = bsObjectNew();
    for (size_t ix = 0; ix < match->groupCount; ix++) {
        BSValue text = bsNull();
        if (match->matched[ix]) {
            size_t begin = bsStringOffset(string, match->groups[ix].begin);
            size_t end = bsStringOffset(string, match->groups[ix].end);
            if (end == begin) {
                text = bsRetain(bsMatchEmpty);
            } else {
                text = bsStringNewSize(bsStringData(string) + begin, end - begin);
            }
        }
        if (ix < 10) {
            bsObjectSetString(groups, bsMatchKeyDigit[ix], text);
        } else {
            char key[8];
            size_t keySize = 0;
            size_t number = ix;
            do {
                key[keySize++] = (char) ('0' + number % 10);
                number /= 10;
            } while (number != 0);
            key[keySize] = '\0';
            for (size_t ixKey = 0; ixKey < keySize / 2; ixKey++) {
                char swap = key[ixKey];
                key[ixKey] = key[keySize - 1 - ixKey];
                key[keySize - 1 - ixKey] = swap;
            }
            bsObjectSet(groups, key, text);
        }

        /* A named group is keyed by both its number and its name */
        const char *name = bsRegexGroupName(regex, ix);
        if (name != NULL) {
            bsObjectSet(groups, name, bsRetain(text));
        }
    }

    BSValue model = bsObjectNew();
    bsObjectSetString(model, bsMatchKeyIndex, bsNumber((double) match->begin));
    bsObjectSetString(model, bsMatchKeyInput, bsRetain(string));
    bsObjectSetString(model, bsMatchKeyGroups, groups);
    (void) subject;
    return model;
}


BSValue bsRegexMatchImpl(BSValue regex, BSValue string)
{
    BSRegexSubject subject;
    bsRegexSubjectInit(&subject, string);
    BSRegexMatch match;
    BSValue result = bsNull();
    if (bsRegexSearch(regex, &subject, 0, &match)) {
        result = bsRegexMatchModel(regex, string, &subject, &match);
    }
    bsRegexSubjectFree(&subject);
    return result;
}


static const BSArgModel regexMatchArgs[] = {
    {"regex", BS_ARG_REGEX, 0, 0, 0, 0, 0},
    {"string", BS_ARG_STRING, 0, 0, 0, 0, 0}
};

static BSValue bsFnRegexMatch(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(regexMatchArgs, 2, args, argCount, values, options, "regexMatch")) {
        return bsNull();
    }
    return bsRegexMatchImpl(values[0], values[1]);
}


static const BSArgModel regexMatchAllArgs[] = {
    {"regex", BS_ARG_REGEX, 0, 0, 0, 0, 0},
    {"string", BS_ARG_STRING, 0, 0, 0, 0, 0}
};

static BSValue bsFnRegexMatchAll(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(regexMatchAllArgs, 2, args, argCount, values, options, "regexMatchAll")) {
        return bsNull();
    }
    BSRegexSubject subject;
    bsRegexSubjectInit(&subject, values[1]);
    BSValue result = bsArrayNew();
    BSRegexMatch match;
    size_t start = 0;
    while (start <= subject.length && bsRegexSearch(values[0], &subject, start, &match)) {
        bsArrayPush(result, bsRegexMatchModel(values[0], values[1], &subject, &match));
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
    BSValue values[2];
    if (!bsArgsValidate(regexNewArgs, 2, args, argCount, values, options, "regexNew")) {
        return bsNull();
    }
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

        /* A numbered group reference */
        if (next >= '0' && next <= '9') {
            size_t end = ix + 1;
            size_t group = 0;
            while (end < substrSize && substr[end] >= '0' && substr[end] <= '9') {
                group = group * 10 + (size_t) (substr[end] - '0');
                end++;
            }
            if (group < match->groupCount && match->matched[group]) {
                size_t begin = bsStringOffset(string, match->groups[group].begin);
                size_t groupEnd = bsStringOffset(string, match->groups[group].end);
                bsSBAppend(sb, bsStringData(string) + begin, groupEnd - begin);
            }
            ix = end - 1;
            continue;
        }

        /* A named group reference */
        if (next == '<') {
            size_t end = ix + 2;
            while (end < substrSize && substr[end] != '>') {
                end++;
            }
            if (end < substrSize) {
                for (size_t group = 1; group < match->groupCount; group++) {
                    const char *name = bsRegexGroupName(regex, group);
                    if (name != NULL && strlen(name) == end - ix - 2 &&
                        memcmp(name, substr + ix + 2, end - ix - 2) == 0) {
                        if (match->matched[group]) {
                            size_t begin = bsStringOffset(string, match->groups[group].begin);
                            size_t groupEnd = bsStringOffset(string, match->groups[group].end);
                            bsSBAppend(sb, bsStringData(string) + begin, groupEnd - begin);
                        }
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
    BSValue values[3];
    if (!bsArgsValidate(regexReplaceArgs, 3, args, argCount, values, options, "regexReplace")) {
        return bsNull();
    }
    BSRegexSubject subject;
    bsRegexSubjectInit(&subject, values[1]);

    BSStringBuilder sb;
    bsSBInit(&sb);
    const char *text = bsStringData(values[1]);
    size_t position = 0;
    BSRegexMatch match;
    while (position <= subject.length && bsRegexSearch(values[0], &subject, position, &match)) {
        size_t matchBegin = bsStringOffset(values[1], match.begin);
        size_t positionOffset = bsStringOffset(values[1], position);
        bsSBAppend(&sb, text + positionOffset, matchBegin - positionOffset);
        bsRegexExpand(&sb, values[0], values[1], &match, bsStringData(values[2]), bsStringSize(values[2]));
        if (match.end > match.begin) {
            position = match.end;
        } else {
            if (match.begin < subject.length) {
                size_t begin = bsStringOffset(values[1], match.begin);
                size_t end = bsStringOffset(values[1], match.begin + 1);
                bsSBAppend(&sb, text + begin, end - begin);
            }
            position = match.begin + 1;
        }
    }
    if (position <= subject.length) {
        size_t positionOffset = bsStringOffset(values[1], position);
        bsSBAppend(&sb, text + positionOffset, bsStringSize(values[1]) - positionOffset);
    }
    bsRegexSubjectFree(&subject);
    return bsSBToValue(&sb);
}


static const BSArgModel regexSplitArgs[] = {
    {"regex", BS_ARG_REGEX, 0, 0, 0, 0, 0},
    {"string", BS_ARG_STRING, 0, 0, 0, 0, 0}
};

static BSValue bsFnRegexSplit(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(regexSplitArgs, 2, args, argCount, values, options, "regexSplit")) {
        return bsNull();
    }
    BSRegexSubject subject;
    bsRegexSubjectInit(&subject, values[1]);

    BSValue result = bsArrayNew();
    const char *text = bsStringData(values[1]);
    size_t position = 0;
    size_t start = 0;
    BSRegexMatch match;
    while (start <= subject.length && bsRegexSearch(values[0], &subject, start, &match)) {
        size_t positionOffset = bsStringOffset(values[1], position);
        size_t matchBegin = bsStringOffset(values[1], match.begin);
        bsArrayPush(result, bsStringNewSize(text + positionOffset, matchBegin - positionOffset));

        /* The capture groups are part of the split result */
        for (size_t ix = 1; ix < match.groupCount; ix++) {
            if (match.matched[ix]) {
                size_t begin = bsStringOffset(values[1], match.groups[ix].begin);
                size_t end = bsStringOffset(values[1], match.groups[ix].end);
                bsArrayPush(result, bsStringNewSize(text + begin, end - begin));
            } else {
                bsArrayPush(result, bsNull());
            }
        }
        position = match.end;
        start = (match.end > match.begin) ? match.end : match.begin + 1;
    }
    size_t positionOffset = bsStringOffset(values[1], position);
    bsArrayPush(result, bsStringNewSize(text + positionOffset, bsStringSize(values[1]) - positionOffset));
    bsRegexSubjectFree(&subject);
    return result;
}


/*
 * String functions
 */


static const BSArgModel stringCharAtArgs[] = {
    {"string", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"index", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_GTE, 0, 0, 0, 0}
};

static BSValue bsFnStringCharAt(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(stringCharAtArgs, 2, args, argCount, values, options, "stringCharAt")) {
        return bsNull();
    }
    size_t index = (size_t) values[1].u.number;
    if (index >= bsStringLength(values[0])) {
        return bsArgFail(options, "index", values[1], bsNull());
    }
    size_t begin = bsStringOffset(values[0], index);
    size_t end = bsStringOffset(values[0], index + 1);
    return bsStringNewSize(bsStringData(values[0]) + begin, end - begin);
}


static const BSArgModel stringCharCodeAtArgs[] = {
    {"string", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"index", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_GTE, 0, 0, 0, 0}
};

static BSValue bsFnStringCharCodeAt(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(stringCharCodeAtArgs, 2, args, argCount, values, options, "stringCharCodeAt")) {
        return bsNull();
    }
    size_t index = (size_t) values[1].u.number;
    if (index >= bsStringLength(values[0])) {
        return bsArgFail(options, "index", values[1], bsNull());
    }
    return bsNumber(bsStringCodePoint(values[0], index));
}


static const BSArgModel stringDecodeArgs[] = {{"bytes", BS_ARG_ARRAY, 0, 0, 0, 0, 0}};

static BSValue bsFnStringDecode(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(stringDecodeArgs, 1, args, argCount, values, options, "stringDecode")) {
        return bsNull();
    }
    size_t count = bsArrayCount(values[0]);
    char *buffer = bsAlloc(count + 1);
    for (size_t ix = 0; ix < count; ix++) {
        BSValue byte = bsArrayGet(values[0], ix);
        if (byte.type != BS_NUMBER || trunc(byte.u.number) != byte.u.number ||
            byte.u.number < 0 || byte.u.number > 255) {
            free(buffer);
            return bsNull();
        }
        buffer[ix] = (char) (unsigned char) byte.u.number;
    }
    buffer[count] = '\0';
    if (bsUTF8Length(buffer, count) == SIZE_MAX) {
        free(buffer);
        return bsNull();
    }
    BSValue result = bsStringNewSize(buffer, count);
    free(buffer);
    return result;
}


static const BSArgModel stringEncodeArgs[] = {{"string", BS_ARG_STRING, 0, 0, 0, 0, 0}};

static BSValue bsFnStringEncode(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(stringEncodeArgs, 1, args, argCount, values, options, "stringEncode")) {
        return bsNull();
    }
    size_t size = bsStringSize(values[0]);
    const char *text = bsStringData(values[0]);
    BSValue result = bsArrayNewCapacity(size);
    for (size_t ix = 0; ix < size; ix++) {
        bsArrayPush(result, bsNumber((unsigned char) text[ix]));
    }
    return result;
}


static const BSArgModel stringEndsWithArgs[] = {
    {"string", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"search", BS_ARG_STRING, 0, 0, 0, 0, 0}
};

static BSValue bsFnStringEndsWith(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(stringEndsWithArgs, 2, args, argCount, values, options, "stringEndsWith")) {
        return bsNull();
    }
    size_t size = bsStringSize(values[0]);
    size_t searchSize = bsStringSize(values[1]);
    return bsBoolean(searchSize <= size &&
                     memcmp(bsStringData(values[0]) + size - searchSize, bsStringData(values[1]),
                            searchSize) == 0);
}


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
        return offset <= size ? offset : SIZE_MAX;
    }
    if (searchSize > size) {
        return SIZE_MAX;
    }
    for (size_t ix = offset; ix + searchSize <= size; ix++) {
        if (text[ix] == search[0] && memcmp(text + ix, search, searchSize) == 0) {
            return ix;
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
    BSValue values[3];
    if (!bsArgsValidate(stringIndexOfArgs, 3, args, argCount, values, options, "stringIndexOf")) {
        return bsNumber(-1);
    }
    size_t index = (size_t) values[2].u.number;
    if (index > bsStringLength(values[0])) {
        return bsArgFail(options, "index", values[2], bsNumber(-1));
    }
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
    BSValue values[3];
    if (!bsArgsValidate(stringLastIndexOfArgs, 3, args, argCount, values, options, "stringLastIndexOf")) {
        return bsNumber(-1);
    }
    size_t length = bsStringLength(values[0]);
    size_t index;
    if (values[2].type == BS_NUMBER) {
        index = (size_t) values[2].u.number;
        if (index > length) {
            return bsArgFail(options, "index", values[2], bsNumber(-1));
        }
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


static const BSArgModel stringLengthArgs[] = {{"string", BS_ARG_STRING, 0, 0, 0, 0, 0}};

static BSValue bsFnStringLength(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(stringLengthArgs, 1, args, argCount, values, options, "stringLength")) {
        return bsNumber(0);
    }
    return bsNumber((double) bsStringLength(values[0]));
}


static const BSArgModel stringCaseArgs[] = {{"string", BS_ARG_STRING, 0, 0, 0, 0, 0}};

static BSValue bsStringCase(const BSValue *args, size_t argCount, BSOptions *options, const char *name,
                            bool upper)
{
    BSValue values[1];
    if (!bsArgsValidate(stringCaseArgs, 1, args, argCount, values, options, name)) {
        return bsNull();
    }
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


static BSValue bsFnStringLower(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    return bsStringCase(args, argCount, options, "stringLower", false);
}


static BSValue bsFnStringUpper(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    return bsStringCase(args, argCount, options, "stringUpper", true);
}


static const BSArgModel stringNewArgs[] = {{"value", BS_ARG_ANY, 0, 0, 0, 0, 0}};

static BSValue bsFnStringNewFn(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(stringNewArgs, 1, args, argCount, values, options, "stringNew")) {
        return bsNull();
    }
    return bsValueString(values[0]);
}


static const BSArgModel stringRepeatArgs[] = {
    {"string", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"count", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_GTE, 0, 0, 0, 0}
};

static BSValue bsFnStringRepeat(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(stringRepeatArgs, 2, args, argCount, values, options, "stringRepeat")) {
        return bsNull();
    }
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
    BSValue values[3];
    if (!bsArgsValidate(stringReplaceArgs, 3, args, argCount, values, options, "stringReplace")) {
        return bsNull();
    }
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
    size_t position = 0;
    while (position <= size) {
        size_t found = bsMemFind(text, size, substr, substrSize, position);
        if (found == SIZE_MAX) {
            break;
        }
        bsSBAppend(&sb, text + position, found - position);
        bsSBAppend(&sb, bsStringData(values[2]), bsStringSize(values[2]));
        position = found + substrSize;
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
    BSValue values[3];
    if (!bsArgsValidate(stringSliceArgs, 3, args, argCount, values, options, "stringSlice")) {
        return bsNull();
    }
    size_t length = bsStringLength(values[0]);
    size_t start = (size_t) values[1].u.number;
    size_t end = values[2].type == BS_NUMBER ? (size_t) values[2].u.number : length;
    if (start > length) {
        return bsArgFail(options, "start", values[1], bsNull());
    }
    if (end > length) {
        return bsArgFail(options, "end", values[2], bsNull());
    }
    if (end < start) {
        return bsStringNewSize("", 0);
    }
    size_t beginOffset = bsStringOffset(values[0], start);
    size_t endOffset = bsStringOffset(values[0], end);
    return bsStringNewSize(bsStringData(values[0]) + beginOffset, endOffset - beginOffset);
}


static const BSArgModel stringSplitArgs[] = {
    {"string", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"separator", BS_ARG_STRING, 0, 0, 0, 0, 0}
};

static BSValue bsFnStringSplit(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(stringSplitArgs, 2, args, argCount, values, options, "stringSplit")) {
        return bsNull();
    }
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


static const BSArgModel stringSplitLinesArgs[] = {{"string", BS_ARG_STRING, 0, 0, 0, 0, 0}};

static BSValue bsFnStringSplitLines(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(stringSplitLinesArgs, 1, args, argCount, values, options, "stringSplitLines")) {
        return bsNull();
    }
    const char *text = bsStringData(values[0]);
    size_t size = bsStringSize(values[0]);
    BSValue result = bsArrayNew();
    size_t position = 0;
    for (size_t ix = 0; ix <= size; ix++) {
        if (ix == size || text[ix] == '\n') {
            size_t end = ix;
            if (end > position && text[end - 1] == '\r') {
                end--;
            }
            bsArrayPush(result, bsStringNewSize(text + position, end - position));
            position = ix + 1;
        }
    }
    return result;
}


static const BSArgModel stringStartsWithArgs[] = {
    {"string", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"search", BS_ARG_STRING, 0, 0, 0, 0, 0}
};

static BSValue bsFnStringStartsWith(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(stringStartsWithArgs, 2, args, argCount, values, options, "stringStartsWith")) {
        return bsNull();
    }
    size_t searchSize = bsStringSize(values[1]);
    return bsBoolean(searchSize <= bsStringSize(values[0]) &&
                     memcmp(bsStringData(values[0]), bsStringData(values[1]), searchSize) == 0);
}


static const BSArgModel stringTrimArgs[] = {{"string", BS_ARG_STRING, 0, 0, 0, 0, 0}};

static BSValue bsFnStringTrim(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(stringTrimArgs, 1, args, argCount, values, options, "stringTrim")) {
        return bsNull();
    }
    const char *text = bsStringData(values[0]);
    size_t begin = 0;
    size_t end = bsStringSize(values[0]);
    while (begin < end && isspace((unsigned char) text[begin])) {
        begin++;
    }
    while (end > begin && isspace((unsigned char) text[end - 1])) {
        end--;
    }
    return bsStringNewSize(text + begin, end - begin);
}


/*
 * System functions
 */


static const BSArgModel systemBooleanArgs[] = {{"value", BS_ARG_ANY, 0, 0, 0, 0, 0}};

static BSValue bsFnSystemBoolean(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(systemBooleanArgs, 1, args, argCount, values, options, "systemBoolean")) {
        return bsNull();
    }
    return bsBoolean(bsValueBoolean(values[0]));
}


static const BSArgModel systemCompareArgs[] = {
    {"left", BS_ARG_ANY, 0, 0, 0, 0, 0},
    {"right", BS_ARG_ANY, 0, 0, 0, 0, 0}
};

static BSValue bsFnSystemCompare(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(systemCompareArgs, 2, args, argCount, values, options, "systemCompare")) {
        return bsNull();
    }
    return bsNumber(bsValueCompare(values[0], values[1]));
}


/* Validate a systemFetch request model - returns false if the model is invalid */
static bool bsFetchRequestValidate(BSValue request)
{
    BSValue url = bsObjectGet(request, "url");
    BSValue body = bsObjectGet(request, "body");
    BSValue headers = bsObjectGet(request, "headers");
    if (url.type != BS_STRING || (body.type != BS_NULL && body.type != BS_STRING) ||
        (headers.type != BS_NULL && headers.type != BS_OBJECT)) {
        return false;
    }
    if (headers.type == BS_OBJECT) {
        BSValue keys = bsObjectKeys(headers);
        bool valid = true;
        for (size_t ix = 0; ix < bsArrayCount(keys) && valid; ix++) {
            valid = bsObjectGetString(headers, bsArrayGet(keys, ix)).type == BS_STRING;
        }
        bsRelease(keys);
        if (!valid) {
            return false;
        }
    }
    return true;
}


/* Fetch one request model - returns the response string value, or null */
static BSValue bsFetchOne(BSValue request, BSOptions *options)
{
    BSValue url = bsObjectGet(request, "url");
    BSValue body = bsObjectGet(request, "body");
    BSValue headers = bsObjectGet(request, "headers");

    BSValue fetchUrl = bsRetain(url);
    if (options->urlFn != NULL) {
        char *resolved = options->urlFn(bsStringData(fetchUrl), options->urlData);
        bsAssign(&fetchUrl, bsStringNew(resolved));
        free(resolved);
    }

    BSValue response = bsNull();
    if (options->fetchFn != NULL) {
        BSFetchRequest fetchRequest;
        memset(&fetchRequest, 0, sizeof(fetchRequest));
        fetchRequest.url = bsStringData(fetchUrl);
        fetchRequest.body = body.type == BS_STRING ? bsStringData(body) : NULL;
        fetchRequest.bodySize = body.type == BS_STRING ? bsStringSize(body) : 0;
        fetchRequest.headers = headers;
        size_t responseSize = 0;
        char *text = options->fetchFn(&fetchRequest, &responseSize, options->fetchData);
        if (text != NULL) {
            response = bsStringNewSize(text, responseSize);
            free(text);
        }
    }
    if (response.type == BS_NULL && options->debug) {
        bsLog(options, "BareScript: Function \"systemFetch\" failed for resource \"%s\"",
              bsStringData(fetchUrl));
    }
    bsRelease(fetchUrl);
    return response;
}


static const BSArgModel systemFetchArgs[] = {{"url", BS_ARG_ANY, 0, 0, 0, 0, 0}};

static BSValue bsFnSystemFetch(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(systemFetchArgs, 1, args, argCount, values, options, "systemFetch")) {
        return bsNull();
    }
    BSValue url = values[0];

    /* A single URL string or request model */
    if (url.type == BS_STRING) {
        BSValue request = bsObjectNew();
        bsObjectSet(request, "url", bsRetain(url));
        BSValue response = bsFetchOne(request, options);
        bsRelease(request);
        return response;
    }
    if (url.type == BS_OBJECT) {
        if (!bsFetchRequestValidate(url)) {
            return bsArgFail(options, "url", url, bsNull());
        }
        return bsFetchOne(url, options);
    }

    /* An array of URL strings and request models */
    if (url.type == BS_ARRAY) {
        size_t count = bsArrayCount(url);
        BSValue responses = bsArrayNewCapacity(count);
        for (size_t ix = 0; ix < count; ix++) {
            BSValue item = bsArrayGet(url, ix);
            if (item.type == BS_STRING) {
                BSValue request = bsObjectNew();
                bsObjectSet(request, "url", bsRetain(item));
                bsArrayPush(responses, bsFetchOne(request, options));
                bsRelease(request);
            } else if (item.type == BS_OBJECT && bsFetchRequestValidate(item)) {
                bsArrayPush(responses, bsFetchOne(item, options));
            } else {
                bsRelease(responses);
                return bsArgFail(options, "url", item, bsNull());
            }
        }
        return responses;
    }

    return bsArgFail(options, "url", url, bsNull());
}


static const BSArgModel systemGlobalGetArgs[] = {
    {"name", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"defaultValue", BS_ARG_ANY, 0, 0, 0, 0, 0}
};

static BSValue bsFnSystemGlobalGet(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(systemGlobalGetArgs, 2, args, argCount, values, options, "systemGlobalGet")) {
        return bsNull();
    }
    if (!bsObjectHasString(options->globals, values[0])) {
        return bsRetain(values[1]);
    }
    return bsRetain(bsObjectGetString(options->globals, values[0]));
}


static const BSArgModel systemGlobalSetArgs[] = {
    {"name", BS_ARG_STRING, 0, 0, 0, 0, 0},
    {"value", BS_ARG_ANY, 0, 0, 0, 0, 0}
};

static BSValue bsFnSystemGlobalSet(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(systemGlobalSetArgs, 2, args, argCount, values, options, "systemGlobalSet")) {
        return bsNull();
    }
    bsObjectSetString(options->globals, values[0], bsRetain(values[1]));
    return bsRetain(values[1]);
}


static const BSArgModel systemIsArgs[] = {
    {"value1", BS_ARG_ANY, 0, 0, 0, 0, 0},
    {"value2", BS_ARG_ANY, 0, 0, 0, 0, 0}
};

static BSValue bsFnSystemIs(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(systemIsArgs, 2, args, argCount, values, options, "systemIs")) {
        return bsNull();
    }
    return bsBoolean(bsValueIs(values[0], values[1]));
}


static const BSArgModel systemLogArgs[] = {{"message", BS_ARG_ANY, 0, 0, 0, 0, 0}};

static BSValue bsFnSystemLog(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(systemLogArgs, 1, args, argCount, values, options, "systemLog")) {
        return bsNull();
    }
    if (options->logFn != NULL) {
        BSValue text = bsValueString(values[0]);
        options->logFn(bsStringData(text), options->logData);
        bsRelease(text);
    }
    return bsNull();
}


static BSValue bsFnSystemLogDebug(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(systemLogArgs, 1, args, argCount, values, options, "systemLogDebug")) {
        return bsNull();
    }
    if (options->logFn != NULL && options->debug) {
        BSValue text = bsValueString(values[0]);
        options->logFn(bsStringData(text), options->logData);
        bsRelease(text);
    }
    return bsNull();
}


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
    BSValue values[2];
    if (!bsArgsValidate(systemPartialArgs, 2, args, argCount, values, options, "systemPartial")) {
        return bsNull();
    }
    if (bsArrayCount(values[1]) < 1) {
        BSValue result = bsArgFail(options, "args", values[1], bsNull());
        bsArgsFree(systemPartialArgs, 2, values);
        return result;
    }
    BSPartial *partial = bsAlloc(sizeof(BSPartial));
    partial->function = bsRetain(values[0]);
    partial->args = bsRetain(values[1]);
    bsArgsFree(systemPartialArgs, 2, values);
    return bsFunctionNew("systemPartial", bsPartialCall, partial, bsPartialFree);
}


static const BSArgModel systemTypeArgs[] = {{"value", BS_ARG_ANY, 0, 0, 0, 0, 0}};

/* Interned once; schemaValidate compares these on every value. */
static BSValue bsSystemTypeNames[BS_REGEX + 1];

static BSValue bsSystemTypeName(BSValue value)
{
    static int ready;
    if (!ready) {
        static const char *const names[] = {
            "null", "boolean", "number", "datetime", "string", "array", "object", "function", "regex"
        };
        for (int ix = 0; ix <= (int) BS_REGEX; ix++) {
            bsSystemTypeNames[ix] = bsStringIntern(names[ix], strlen(names[ix]));
        }
        ready = 1;
    }
    return bsRetain(bsSystemTypeNames[value.type]);
}

static BSValue bsFnSystemType(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[1];
    if (!bsArgsValidate(systemTypeArgs, 1, args, argCount, values, options, "systemType")) {
        return bsNull();
    }
    return bsSystemTypeName(values[0]);
}


/*
 * The library function tables
 */


typedef struct BSLibraryEntry {
    const char *name;
    BSFunctionFn fn;
} BSLibraryEntry;


static const BSLibraryEntry bsScriptFunctionTable[] = {
    {"arrayCopy", bsFnArrayCopy},
    {"arrayDelete", bsFnArrayDelete},
    {"arrayExtend", bsFnArrayExtend},
    {"arrayFlat", bsFnArrayFlat},
    {"arrayGet", bsFnArrayGet},
    {"arrayIndexOf", bsFnArrayIndexOf},
    {"arrayJoin", bsFnArrayJoin},
    {"arrayLastIndexOf", bsFnArrayLastIndexOf},
    {"arrayLength", bsFnArrayLength},
    {"arrayNew", bsFnArrayNew},
    {"arrayNewSize", bsFnArrayNewSize},
    {"arrayPop", bsFnArrayPop},
    {"arrayPush", bsFnArrayPush},
    {"arrayReverse", bsFnArrayReverse},
    {"arraySet", bsFnArraySet},
    {"arrayShift", bsFnArrayShift},
    {"arraySlice", bsFnArraySlice},
    {"arraySort", bsFnArraySort},
    {"barescriptEvaluateExpression", bsFnBarescriptEvaluateExpression},
    {"datetimeDay", bsFnDatetimeDay},
    {"datetimeHour", bsFnDatetimeHour},
    {"datetimeISOFormat", bsFnDatetimeISOFormat},
    {"datetimeISOParse", bsFnDatetimeISOParse},
    {"datetimeMillisecond", bsFnDatetimeMillisecond},
    {"datetimeMinute", bsFnDatetimeMinute},
    {"datetimeMonth", bsFnDatetimeMonth},
    {"datetimeNew", bsFnDatetimeNew},
    {"datetimeNow", bsFnDatetimeNow},
    {"datetimeSecond", bsFnDatetimeSecond},
    {"datetimeToday", bsFnDatetimeToday},
    {"datetimeYear", bsFnDatetimeYear},
    {"jsonParse", bsFnJSONParse},
    {"jsonStringify", bsFnJSONStringify},
    {"mathAbs", bsFnMathAbs},
    {"mathAcos", bsFnMathAcos},
    {"mathAsin", bsFnMathAsin},
    {"mathAtan", bsFnMathAtan},
    {"mathAtan2", bsFnMathAtan2},
    {"mathCeil", bsFnMathCeil},
    {"mathCos", bsFnMathCos},
    {"mathE", bsFnMathE},
    {"mathFloor", bsFnMathFloor},
    {"mathLn", bsFnMathLn},
    {"mathLog", bsFnMathLog},
    {"mathMax", bsFnMathMax},
    {"mathMin", bsFnMathMin},
    {"mathPi", bsFnMathPi},
    {"mathRandom", bsFnMathRandom},
    {"mathRound", bsFnMathRound},
    {"mathSign", bsFnMathSign},
    {"mathSin", bsFnMathSin},
    {"mathSqrt", bsFnMathSqrt},
    {"mathTan", bsFnMathTan},
    {"numberParseFloat", bsFnNumberParseFloat},
    {"numberParseInt", bsFnNumberParseInt},
    {"numberToFixed", bsFnNumberToFixed},
    {"numberToString", bsFnNumberToString},
    {"objectAssign", bsFnObjectAssign},
    {"objectCopy", bsFnObjectCopy},
    {"objectDelete", bsFnObjectDelete},
    {"objectGet", bsFnObjectGet},
    {"objectHas", bsFnObjectHas},
    {"objectKeys", bsFnObjectKeys},
    {"objectNew", bsFnObjectNew},
    {"objectSet", bsFnObjectSet},
    {"regexEscape", bsFnRegexEscape},
    {"regexMatch", bsFnRegexMatch},
    {"regexMatchAll", bsFnRegexMatchAll},
    {"regexNew", bsFnRegexNew},
    {"regexReplace", bsFnRegexReplace},
    {"regexSplit", bsFnRegexSplit},
    {"stringCharAt", bsFnStringCharAt},
    {"stringCharCodeAt", bsFnStringCharCodeAt},
    {"stringDecode", bsFnStringDecode},
    {"stringEncode", bsFnStringEncode},
    {"stringEndsWith", bsFnStringEndsWith},
    {"stringFromCharCode", bsFnStringFromCharCode},
    {"stringIndexOf", bsFnStringIndexOf},
    {"stringLastIndexOf", bsFnStringLastIndexOf},
    {"stringLength", bsFnStringLength},
    {"stringLower", bsFnStringLower},
    {"stringNew", bsFnStringNewFn},
    {"stringRepeat", bsFnStringRepeat},
    {"stringReplace", bsFnStringReplace},
    {"stringSlice", bsFnStringSlice},
    {"stringSplit", bsFnStringSplit},
    {"stringSplitLines", bsFnStringSplitLines},
    {"stringStartsWith", bsFnStringStartsWith},
    {"stringTrim", bsFnStringTrim},
    {"stringUpper", bsFnStringUpper},
    {"systemBoolean", bsFnSystemBoolean},
    {"systemCompare", bsFnSystemCompare},
    {"systemFetch", bsFnSystemFetch},
    {"systemGlobalGet", bsFnSystemGlobalGet},
    {"systemGlobalSet", bsFnSystemGlobalSet},
    {"systemIs", bsFnSystemIs},
    {"systemLog", bsFnSystemLog},
    {"systemLogDebug", bsFnSystemLogDebug},
    {"systemPartial", bsFnSystemPartial},
    {"systemType", bsFnSystemType}
};

#define BS_SCRIPT_FUNCTION_COUNT (sizeof(bsScriptFunctionTable) / sizeof(bsScriptFunctionTable[0]))


/* The built-in expression function aliases */
static const struct {
    const char *name;
    const char *scriptName;
} bsExpressionFunctionTable[] = {
    {"abs", "mathAbs"},
    {"acos", "mathAcos"},
    {"arrayNew", "arrayNew"},
    {"asin", "mathAsin"},
    {"atan", "mathAtan"},
    {"atan2", "mathAtan2"},
    {"ceil", "mathCeil"},
    {"charCodeAt", "stringCharCodeAt"},
    {"cos", "mathCos"},
    {"date", "datetimeNew"},
    {"day", "datetimeDay"},
    {"endsWith", "stringEndsWith"},
    {"fixed", "numberToFixed"},
    {"floor", "mathFloor"},
    {"fromCharCode", "stringFromCharCode"},
    {"hour", "datetimeHour"},
    {"indexOf", "stringIndexOf"},
    {"lastIndexOf", "stringLastIndexOf"},
    {"len", "stringLength"},
    {"ln", "mathLn"},
    {"log", "mathLog"},
    {"lower", "stringLower"},
    {"max", "mathMax"},
    {"millisecond", "datetimeMillisecond"},
    {"min", "mathMin"},
    {"minute", "datetimeMinute"},
    {"month", "datetimeMonth"},
    {"now", "datetimeNow"},
    {"objectNew", "objectNew"},
    {"parseFloat", "numberParseFloat"},
    {"parseInt", "numberParseInt"},
    {"pi", "mathPi"},
    {"rand", "mathRandom"},
    {"replace", "stringReplace"},
    {"rept", "stringRepeat"},
    {"round", "mathRound"},
    {"second", "datetimeSecond"},
    {"sign", "mathSign"},
    {"sin", "mathSin"},
    {"slice", "stringSlice"},
    {"sqrt", "mathSqrt"},
    {"startsWith", "stringStartsWith"},
    {"tan", "mathTan"},
    {"text", "stringNew"},
    {"today", "datetimeToday"},
    {"trim", "stringTrim"},
    {"upper", "stringUpper"},
    {"year", "datetimeYear"}
};

#define BS_EXPRESSION_FUNCTION_COUNT \
    (sizeof(bsExpressionFunctionTable) / sizeof(bsExpressionFunctionTable[0]))


/*
 * The library function value cache
 *
 * Library function values are created once and shared by every globals object, so a library
 * function has a stable identity - which systemIs relies on, and which lets an override be
 * detected by pointer inequality.
 */
static BSValue bsScriptFunctionValues = {BS_NULL, {0}};
static BSValue bsExpressionFunctionValues = {BS_NULL, {0}};


static const struct {
    const char *name;
    unsigned char id;
} bsIntrinsicTable[] = {
    {"arrayCopy", BS_INTRIN_ARRAY_COPY},
    {"arrayGet", BS_INTRIN_ARRAY_GET},
    {"arrayLength", BS_INTRIN_ARRAY_LENGTH},
    {"arrayPop", BS_INTRIN_ARRAY_POP},
    {"arrayPush", BS_INTRIN_ARRAY_PUSH},
    {"arraySet", BS_INTRIN_ARRAY_SET},
    {"arrayNew", BS_INTRIN_ARRAY_NEW},
    {"mathAbs", BS_INTRIN_MATH_ABS},
    {"mathCeil", BS_INTRIN_MATH_CEIL},
    {"mathFloor", BS_INTRIN_MATH_FLOOR},
    {"mathSign", BS_INTRIN_MATH_SIGN},
    {"mathSqrt", BS_INTRIN_MATH_SQRT},
    {"objectCopy", BS_INTRIN_OBJECT_COPY},
    {"objectDelete", BS_INTRIN_OBJECT_DELETE},
    {"objectGet", BS_INTRIN_OBJECT_GET},
    {"objectHas", BS_INTRIN_OBJECT_HAS},
    {"objectKeys", BS_INTRIN_OBJECT_KEYS},
    {"objectSet", BS_INTRIN_OBJECT_SET},
    {"objectNew", BS_INTRIN_OBJECT_NEW},
    {"stringEndsWith", BS_INTRIN_STRING_ENDS_WITH},
    {"stringLength", BS_INTRIN_STRING_LENGTH},
    {"stringStartsWith", BS_INTRIN_STRING_STARTS_WITH},
    {"systemBoolean", BS_INTRIN_SYSTEM_BOOLEAN},
    {"systemType", BS_INTRIN_SYSTEM_TYPE},
    {"regexMatch", BS_INTRIN_REGEX_MATCH}
};

#define BS_INTRINSIC_COUNT (sizeof(bsIntrinsicTable) / sizeof(bsIntrinsicTable[0]))


static bool bsFastIndex(BSValue value, size_t *index)
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


static void bsLibraryInit(void)
{
    if (bsScriptFunctionValues.type == BS_OBJECT) {
        return;
    }
    bsScriptFunctionValues = bsObjectNew();
    for (size_t ix = 0; ix < BS_SCRIPT_FUNCTION_COUNT; ix++) {
        bsObjectSet(bsScriptFunctionValues, bsScriptFunctionTable[ix].name,
                    bsFunctionNew(bsScriptFunctionTable[ix].name, bsScriptFunctionTable[ix].fn, NULL, NULL));
    }
    for (size_t ix = 0; ix < BS_INTRINSIC_COUNT; ix++) {
        BSValue function = bsObjectGet(bsScriptFunctionValues, bsIntrinsicTable[ix].name);
        function.u.function->intrinsic = bsIntrinsicTable[ix].id;
    }
    bsExpressionFunctionValues = bsObjectNew();
    for (size_t ix = 0; ix < BS_EXPRESSION_FUNCTION_COUNT; ix++) {
        bsObjectSet(bsExpressionFunctionValues, bsExpressionFunctionTable[ix].name,
                    bsRetain(bsObjectGet(bsScriptFunctionValues, bsExpressionFunctionTable[ix].scriptName)));
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
    bsRelease(bsExpressionFunctionValues);
    bsRelease(bsScriptFunctionValues);
    bsExpressionFunctionValues = bsNull();
    bsScriptFunctionValues = bsNull();
}


BSValue bsFunctionInvoke(BSValue function, const BSValue *args, size_t argCount, BSOptions *options)
{
    BSFunction *fn = function.u.function;
    switch (fn->intrinsic) {
    case BS_INTRIN_ARRAY_COPY:
        if (argCount == 1 && args[0].type == BS_ARRAY) {
            return bsArrayCopy(args[0]);
        }
        break;
    case BS_INTRIN_ARRAY_GET: {
        size_t index;
        if (argCount == 2 && args[0].type == BS_ARRAY && bsFastIndex(args[1], &index) &&
            index < args[0].u.array->count) {
            return bsRetain(args[0].u.array->values[index]);
        }
        break;
    }
    case BS_INTRIN_ARRAY_LENGTH:
        if (argCount == 1 && args[0].type == BS_ARRAY) {
            return bsNumber((double) args[0].u.array->count);
        }
        break;
    case BS_INTRIN_ARRAY_POP:
        if (argCount == 1 && args[0].type == BS_ARRAY && args[0].u.array->count != 0) {
            size_t index = args[0].u.array->count - 1;
            BSValue result = bsRetain(args[0].u.array->values[index]);
            bsArrayDelete(args[0], index);
            return result;
        }
        break;
    case BS_INTRIN_ARRAY_PUSH:
        if (argCount >= 1 && args[0].type == BS_ARRAY) {
            for (size_t ix = 1; ix < argCount; ix++) {
                bsArrayPush(args[0], bsRetain(args[ix]));
            }
            return bsRetain(args[0]);
        }
        break;
    case BS_INTRIN_ARRAY_SET: {
        size_t index;
        if (argCount == 3 && args[0].type == BS_ARRAY && bsFastIndex(args[1], &index) &&
            index < args[0].u.array->count) {
            bsArraySet(args[0], index, bsRetain(args[2]));
            return bsRetain(args[2]);
        }
        break;
    }
    case BS_INTRIN_MATH_ABS:
        if (argCount == 1 && args[0].type == BS_NUMBER) {
            return bsNumber(fabs(args[0].u.number));
        }
        break;
    case BS_INTRIN_MATH_CEIL:
        if (argCount == 1 && args[0].type == BS_NUMBER) {
            return bsNumber(ceil(args[0].u.number));
        }
        break;
    case BS_INTRIN_MATH_FLOOR:
        if (argCount == 1 && args[0].type == BS_NUMBER) {
            return bsNumber(floor(args[0].u.number));
        }
        break;
    case BS_INTRIN_MATH_SIGN:
        if (argCount == 1 && args[0].type == BS_NUMBER) {
            double x = args[0].u.number;
            return bsNumber(x < 0 ? -1 : (x == 0 ? 0 : 1));
        }
        break;
    case BS_INTRIN_MATH_SQRT:
        if (argCount == 1 && args[0].type == BS_NUMBER && args[0].u.number >= 0) {
            return bsNumber(sqrt(args[0].u.number));
        }
        break;
    case BS_INTRIN_OBJECT_COPY:
        if (argCount == 1 && args[0].type == BS_OBJECT) {
            return bsObjectCopy(args[0]);
        }
        break;
    case BS_INTRIN_OBJECT_DELETE:
        if (argCount == 2 && args[0].type == BS_OBJECT && args[1].type == BS_STRING) {
            bsObjectDelete(args[0], bsStringData(args[1]));
            return bsNull();
        }
        break;
    case BS_INTRIN_OBJECT_GET:
        if (argCount >= 2 && argCount <= 3 && args[0].type == BS_OBJECT && args[1].type == BS_STRING) {
            BSValue found;
            if (bsObjectLookupString(args[0], args[1], &found)) {
                return bsRetain(found);
            }
            return bsRetain(argCount >= 3 ? args[2] : bsNull());
        }
        break;
    case BS_INTRIN_OBJECT_HAS:
        if (argCount == 2 && args[0].type == BS_OBJECT && args[1].type == BS_STRING) {
            return bsBoolean(bsObjectHasString(args[0], args[1]));
        }
        break;
    case BS_INTRIN_OBJECT_KEYS:
        if (argCount == 1 && args[0].type == BS_OBJECT) {
            return bsObjectKeys(args[0]);
        }
        break;
    case BS_INTRIN_OBJECT_SET:
        if (argCount == 3 && args[0].type == BS_OBJECT && args[1].type == BS_STRING) {
            bsObjectSetString(args[0], args[1], bsRetain(args[2]));
            return bsRetain(args[2]);
        }
        break;
    case BS_INTRIN_STRING_ENDS_WITH:
        if (argCount == 2 && args[0].type == BS_STRING && args[1].type == BS_STRING) {
            size_t searchSize = args[1].u.string->size;
            size_t size = args[0].u.string->size;
            return bsBoolean(searchSize <= size &&
                             memcmp(args[0].u.string->data + (size - searchSize),
                                    args[1].u.string->data, searchSize) == 0);
        }
        break;
    case BS_INTRIN_STRING_LENGTH:
        if (argCount == 1 && args[0].type == BS_STRING) {
            return bsNumber((double) args[0].u.string->length);
        }
        break;
    case BS_INTRIN_STRING_STARTS_WITH:
        if (argCount == 2 && args[0].type == BS_STRING && args[1].type == BS_STRING) {
            size_t searchSize = args[1].u.string->size;
            return bsBoolean(searchSize <= args[0].u.string->size &&
                             memcmp(args[0].u.string->data, args[1].u.string->data, searchSize) == 0);
        }
        break;
    case BS_INTRIN_SYSTEM_BOOLEAN:
        if (argCount == 1) {
            return bsBoolean(bsValueBoolean(args[0]));
        }
        break;
    case BS_INTRIN_SYSTEM_TYPE:
        if (argCount == 1) {
            return bsSystemTypeName(args[0]);
        }
        break;
    case BS_INTRIN_REGEX_MATCH:
        if (argCount == 2 && args[0].type == BS_REGEX && args[1].type == BS_STRING) {
            return bsRegexMatchImpl(args[0], args[1]);
        }
        break;
    default:
        break;
    }
    return fn->fn(args, argCount, options, fn->data);
}
