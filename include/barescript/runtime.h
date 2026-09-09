/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The BareScript runtime
 */

#ifndef BARESCRIPT_RUNTIME_H
#define BARESCRIPT_RUNTIME_H

#include "parser.h"
#include "value.h"
#include "export.h"

#ifdef __cplusplus
extern "C" {
#endif

BS_VISIBILITY_BEGIN


/* The default maximum number of statements executed by bsExecuteScript */
#define BS_MAX_STATEMENTS_DEFAULT 1000000000

/* The coverage configuration global variable name */
#define BS_GLOBAL_COVERAGE "__barescriptCoverage"

/* The includes-loaded global variable name */
#define BS_GLOBAL_INCLUDES "__barescriptIncludes"


/*
 * A fetch request
 */
typedef struct BSFetchRequest {
    const char *url;
    const char *body;  /* the request body, or NULL for a GET request */
    size_t bodySize;
    BSValue headers;   /* an object of string header values, or a null value */
} BSFetchRequest;

/*
 * The fetch function signature
 *
 * Fetches "count" requests at once - so an implementation can fetch them concurrently - and sets
 * each successful request's response to its text, an owned string value. The responses arrive as
 * null values, so a request that fails may be left as it is. The caller releases each response.
 */
typedef void (*BSFetchFn)(const BSFetchRequest *requests, BSValue *responses, size_t count, void *data);

/* The log function signature */
typedef void (*BSLogFn)(const char *text, void *data);

/*
 * The URL function signature - resolves an include or fetch URL relative to the current script
 *
 * Returns the resolved URL as a NUL-terminated, malloc-allocated buffer that the caller frees.
 */
typedef char *(*BSUrlFn)(const char *url, void *data);


/*
 * The script execution options
 *
 * Create with bsOptionsNew and destroy with bsOptionsFree. The options own the globals object.
 */
struct BSOptions {
    BSValue globals;      /* the global variables object */
    BSFetchFn fetchFn;
    void *fetchData;
    BSLogFn logFn;
    void *logData;
    BSUrlFn urlFn;
    void *urlData;
    void (*urlDataFree)(void *data);
    bool debug;
    int64_t maxStatements;
    int64_t statementCount;

    /* The pending runtime error, or a null value. Set with bsErrorSet. */
    BSValue error;

    /*
     * The pending library function argument error, or a null value
     *
     * A library function reports an invalid argument by setting this and returning its documented
     * error value. The expression evaluator logs it - with the call site's script name, line
     * number, and function name - when debug logging is on, then clears it.
     */
    BSValue argsError;

    /* The expression evaluation recursion depth guard */
    int depth;
    int depthMax;

    /*
     * Identity for call-site function caches. Unique per options instance so a cached include
     * script cannot reuse a function pointer from a previous, already-freed globals object.
     */
    uint32_t cacheEpoch;

    /*
     * The __barescriptCoverage global's value slot, re-resolved when the globals object changes
     * shape - a script without coverage then pays two compares per function call, not a lookup
     */
    BSValue *coverageSlot;
    uint32_t coverageGen;
    uint32_t coverageEpoch;
};


BSOptions *bsOptionsNew(void);
void bsOptionsFree(BSOptions *options);

/* Set the pending runtime error, which halts script execution */
void bsErrorSet(BSOptions *options, const char *format, ...);

/* Set the pending runtime error, prefixed with a script location. lineNumber 0 omits the line. */
void bsErrorSetStatement(BSOptions *options, const BSScript *script, int lineNumber,
                         const char *format, ...);

/*
 * Set the pending library function error, which does not halt the script
 *
 * A library function that fails for a reason its argument model cannot express - an unparseable
 * JSON string, an invalid regular expression - reports it here and returns its documented error
 * value. The expression evaluator logs it with the call site's location in debug mode.
 */
void bsFunctionError(BSOptions *options, const char *format, ...);

/* Get the pending runtime error message, or NULL if there is none */
const char *bsErrorGet(const BSOptions *options);

/* Clear the pending runtime error */
void bsErrorClear(BSOptions *options);

/* Log a message */
void bsLog(BSOptions *options, const char *format, ...);


/*
 * Execute a parsed script. Returns the script result as an owned value. On a runtime error the
 * result is a null value and bsErrorGet returns the error message.
 */
BSValue bsExecuteScript(BSScript *script, BSOptions *options);


/*
 * The expression evaluation scope
 *
 * A compiled function body uses the "slots" array, indexed by the slot numbers the parser
 * resolved. An expression evaluated against a caller-supplied locals object uses "object".
 * A slot holding the internal unset marker falls through to the globals object, matching the
 * reference implementation's "name not in locals" behavior.
 */
typedef struct BSScope {
    BSValue *slots;
    BSValue object;
} BSScope;

/* Initialize an empty scope - the global scope */
void bsScopeInit(BSScope *scope);


/*
 * Evaluate an expression. "scope" may be NULL for the global scope. If "builtins" is true, the
 * built-in expression function aliases (min, max, len, ...) are in scope.
 */
BSValue bsEvaluateExpression(BSExpr *expr, BSOptions *options, BSScope *scope, bool builtins);


/*
 * Evaluate an expression model object - the JSON "Expression" model the reference implementations
 * use. Returns an owned value.
 */
BSValue bsEvaluateExpressionModel(BSValue exprModel, BSOptions *options, BSValue locals, bool builtins);


/*
 * The system include library
 *
 * System includes - "include <name.bare>" - resolve against a registry of named scripts, then
 * against the registered search path directories. Both are empty unless the host adds to them, so
 * a system include fails by default. The command-line interface adds every directory in the
 * BARESCRIPT_INCLUDE_PATH environment variable to the search path. The registry and search path
 * belong to the calling thread.
 */
void bsSystemIncludeRegister(const char *name, const char *text);
void bsSystemIncludePath(const char *directory);
const char *bsSystemIncludeGet(const char *name);
void bsSystemIncludeClear(void);


/*
 * The bundled BareScript include library
 *
 * The library embeds the BareScript include library - args.bare, markdown.bare, schema.bare, and
 * the rest - as compressed, parser-compiled binary script models, so "include <name.bare>" resolves
 * without a file system. See barescript/includeSource.h for the per-include stub accessors.
 */

/* The number of bundled include library scripts */
size_t bsIncludeCount(void);

/* A bundled include library script's name, by index; NULL if the index is out of range */
const char *bsIncludeName(size_t index);

/*
 * A bundled include library script's binary script model - the encoding bin/includeSource.bare
 * describes - and its size, by name; NULL with a size of zero if there is no such script
 */
const unsigned char *bsIncludeSource(const char *name, size_t *size);

/* Release the calling thread's compiled and decoded bundled include library models */
void bsIncludeCleanup(void);


BS_VISIBILITY_END

#ifdef __cplusplus
}
#endif

#endif
