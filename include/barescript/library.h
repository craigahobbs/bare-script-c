/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The BareScript library
 */

#ifndef BARESCRIPT_LIBRARY_H
#define BARESCRIPT_LIBRARY_H

#include "runtime.h"
#include "value.h"
#include "export.h"

#ifdef __cplusplus
extern "C" {
#endif

BS_VISIBILITY_BEGIN


/*
 * The function argument model
 *
 * Library functions declare their arguments with an argument model and validate them with
 * bsArgsValidate, which coerces and range-checks arguments exactly as the reference
 * implementation's value_args_validate does.
 */
typedef enum {
    BS_ARG_ANY = 0,
    BS_ARG_NUMBER,
    BS_ARG_STRING,
    BS_ARG_ARRAY,
    BS_ARG_OBJECT,
    BS_ARG_DATETIME,
    BS_ARG_REGEX,
    BS_ARG_FUNCTION,
    BS_ARG_BOOLEAN
} BSArgType;

/* Argument model flags */
#define BS_ARG_NULLABLE      0x01 /* the argument may be null */
#define BS_ARG_INTEGER       0x02 /* the number argument must be an integer */
#define BS_ARG_LAST_ARRAY    0x04 /* collect the remaining arguments into an array */
#define BS_ARG_HAS_DEFAULT   0x08 /* the argument has a default number value */
#define BS_ARG_LT            0x10
#define BS_ARG_LTE           0x20
#define BS_ARG_GT            0x40
#define BS_ARG_GTE           0x80

typedef struct BSArgModel {
    const char *name;
    BSArgType type;
    unsigned flags;
    double defaultValue;
    double limit;      /* the lt/lte/gt/gte limit */
    double limit2;     /* a second limit, for arguments with both a lower and upper bound */
    unsigned flags2;   /* the second limit's flag */
} BSArgModel;


/*
 * Validate a function's arguments against an argument model
 *
 * On success, "values" is filled in with argModelCount validated argument values - borrowed
 * references, except for a BS_ARG_LAST_ARRAY argument, which is a newly created owned array - and
 * true is returned. On failure, the argument error is reported through "options" (a debug log
 * message, not a runtime error) and false is returned.
 */
bool bsArgsValidate(const BSArgModel *argModel, size_t argModelCount, const BSValue *args, size_t argCount,
                    BSValue *values, BSOptions *options);

/* Release the validated argument values that bsArgsValidate created */
void bsArgsFree(const BSArgModel *argModel, size_t argModelCount, BSValue *values);


/* Add the BareScript library's script functions to a globals object */
void bsLibraryGlobals(BSValue globals);

/* Look up a built-in expression function alias (min, max, len, ...); returns a borrowed value */
BSValue bsLibraryExpressionFunction(BSValue name);

/* Look up a built-in script function by name; returns a borrowed value */
BSValue bsLibraryScriptFunction(const char *name);

/* Release the calling thread's library function values and HTTP connection pool - call at thread
   exit, before bsValueCleanup */
void bsLibraryCleanup(void);


BS_VISIBILITY_END

#ifdef __cplusplus
}
#endif

#endif
