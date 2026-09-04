/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The BareScript parser and linter
 *
 * BareScript is parsed by barescriptParser.bare and linted by barescriptLint.bare, the same
 * include library scripts the JavaScript and Python implementations use, running on this runtime.
 * Both are bootstrapped straight from their bundled, parser-compiled JSON script models - there is
 * no parser to run before the parser exists - and their output models are converted to the
 * runtime's compiled representation by model.c.
 */

#include <stdlib.h>
#include <string.h>

#include "barescript/json.h"
#include "barescript/library.h"
#include "barescript/parser.h"
#include "barescript/runtime.h"

#include "internal.h"


/*
 * The include library script globals, lazily initialized per thread
 *
 * Each bootstrapped script gets its own globals and options, so parsing and linting never disturb
 * - or are disturbed by - the script being parsed.
 */
typedef struct BSBootstrap {
    const char *includeName;
    BSOptions *options;
    BSScript *script;
} BSBootstrap;

static _Thread_local BSBootstrap bsParserBootstrap = {"barescriptParser.bare", NULL, NULL};
static _Thread_local BSBootstrap bsLintBootstrap = {"barescriptLint.bare", NULL, NULL};


/* Load a bundled include library script and execute it, returning its globals */
static BSValue bsBootstrapGlobals(BSBootstrap *bootstrap)
{
    if (bootstrap->options != NULL) {
        return bootstrap->options->globals;
    }

    /*
     * The bundled model is JSON, so it loads without a parser - which is what makes it possible
     * for the parser itself to be a BareScript script. The compiled script is cached.
     */
    BSScript *script = bsIncludeScript(bootstrap->includeName);
    if (script == NULL) {
        return bsNull(); /* GCOV_EXCL_LINE - parser and linter models always load */
    }

    BSOptions *options = bsOptionsNew();
    bsRelease(bsExecuteScript(script, options));

    bootstrap->options = options;
    bootstrap->script = script;
    return options->globals;
}


void bsParserCleanup(void)
{
    BSBootstrap *bootstraps[2] = {&bsParserBootstrap, &bsLintBootstrap};
    for (size_t ix = 0; ix < 2; ix++) {
        bsOptionsFree(bootstraps[ix]->options);
        bsScriptRelease(bootstraps[ix]->script);
        bootstraps[ix]->options = NULL;
        bootstraps[ix]->script = NULL;
    }
}


void bsParserErrorFree(BSParserError *error)
{
    bsRelease(error->error);
    bsRelease(error->line);
    bsRelease(error->scriptName);
    bsRelease(error->message);
    memset(error, 0, sizeof(*error));
}


/* Fill in a parser error from the parser's error object */
static void bsParserErrorFromModel(BSParserError *error, BSValue errorModel)
{
    if (error == NULL) {
        return;
    }
    BSValue columnNumber = bsObjectGet(errorModel, "columnNumber");
    BSValue lineNumber = bsObjectGet(errorModel, "lineNumber");
    error->error = bsRetain(bsObjectGet(errorModel, "error"));
    error->line = bsRetain(bsObjectGet(errorModel, "line"));
    error->scriptName = bsRetain(bsObjectGet(errorModel, "scriptName"));
    error->message = bsRetain(bsObjectGet(errorModel, "message"));
    error->columnNumber = columnNumber.type == BS_NUMBER ? (int) columnNumber.u.number : 0;
    error->lineNumber = lineNumber.type == BS_NUMBER ? (int) lineNumber.u.number : 0;
}


/*
 * Report a parse failure that is not the parser's own error object
 *
 * A runtime error inside the parser - a script nested deeper than the evaluator's expression depth
 * limit, say - is reported against the script being parsed. The parser's own script name and line
 * number are internal detail, so a leading location prefix is dropped.
 */
static void bsParserErrorInternal(BSParserError *error, const char *scriptName, const char *includeName,
                                  const char *description)
{
    if (error == NULL) {
        return;
    }
    size_t includeSize = strlen(includeName);
    if (strncmp(description, includeName, includeSize) == 0) {
        const char *rest = strchr(description + includeSize, ' ');
        if (rest != NULL) {
            description = rest + 1;
        }
    }
    error->error = bsStringNew(description);
    error->line = bsStringNewSize("", 0);
    error->scriptName = scriptName != NULL ? bsStringNew(scriptName) : bsNull();
    error->message = scriptName != NULL ? bsStringNewFormat("%s: %s\n", scriptName, description) :
        bsStringNewFormat("%s\n", description);
}


/*
 * Call an include library script function, returning its result. Sets a parser error and returns a
 * null value if the call fails.
 */
static BSValue bsParserCall(BSBootstrap *bootstrap, const char *functionName, const BSValue *args,
                            size_t argCount, const char *scriptName, BSParserError *error)
{
    BSValue globals = bsBootstrapGlobals(bootstrap);
    BSValue function = bsObjectGet(globals, functionName);
    BSValue result = bsFunctionCall(function, args, argCount, bootstrap->options);
    const char *runtimeError = bsErrorGet(bootstrap->options);
    if (runtimeError != NULL) {
        bsParserErrorInternal(error, scriptName, bootstrap->includeName, runtimeError);
        bsErrorClear(bootstrap->options);
        bsRelease(result);
        return bsNull();
    }
    return result;
}


/* Unwrap the parser's {result} or {error} object. Returns an owned model, or a null value. */
static BSValue bsParserUnwrap(BSValue result, BSParserError *error)
{
    if (result.type != BS_OBJECT) {
        bsRelease(result);
        return bsNull();
    }
    if (bsObjectHas(result, "error")) {
        bsParserErrorFromModel(error, bsObjectGet(result, "error"));
        bsRelease(result);
        return bsNull();
    }
    BSValue model = bsRetain(bsObjectGet(result, "result"));
    bsRelease(result);
    return model;
}


BSScript *bsParseScript(const char *text, size_t size, int startLineNumber, const char *scriptName,
                        BSParserError *error)
{
    BSValue args[3];
    args[0] = bsStringNewSize(text, size);
    args[1] = bsNumber(startLineNumber);
    args[2] = scriptName != NULL ? bsStringNew(scriptName) : bsNull();

    BSValue result = bsParserCall(&bsParserBootstrap, "barescriptParseScriptEx", args, 3, scriptName,
                                  error);
    bsRelease(args[0]);
    bsRelease(args[2]);
    BSValue model = bsParserUnwrap(result, error);
    if (model.type != BS_OBJECT) {
        return NULL;
    }

    BSScript *script = bsScriptFromModel(model, scriptName);
    bsRelease(model);
    /* GCOV_EXCL_START - the bundled parser cannot produce a model the converter rejects */
    if (script == NULL) {
        bsParserErrorInternal(error, scriptName, bsParserBootstrap.includeName, "Invalid BareScript model");
        return NULL;
    }
    /* GCOV_EXCL_STOP */
    script->startLineNumber = startLineNumber;
    return script;
}


BSValue bsScriptReparse(const BSScript *script)
{
    BSValue args[3];
    args[0] = script->scriptLines;
    args[1] = bsNumber(script->startLineNumber);
    args[2] = script->scriptName;
    BSValue result = bsParserCall(&bsParserBootstrap, "barescriptParseScriptEx", args, 3, NULL, NULL);
    return bsParserUnwrap(result, NULL);
}


BSExpr *bsParseExpression(const char *text, size_t size, int lineNumber, const char *scriptName,
                          bool arrayLiterals, BSParserError *error)
{
    BSValue args[4];
    args[0] = bsStringNewSize(text, size);
    args[1] = lineNumber != 0 ? bsNumber(lineNumber) : bsNull();
    args[2] = scriptName != NULL ? bsStringNew(scriptName) : bsNull();
    args[3] = bsBoolean(arrayLiterals);

    BSValue result = bsParserCall(&bsParserBootstrap, "barescriptParseExpressionEx", args, 4,
                                  scriptName, error);
    bsRelease(args[0]);
    bsRelease(args[2]);
    BSValue model = bsParserUnwrap(result, error);
    if (model.type != BS_OBJECT) {
        return NULL;
    }

    BSExpr *expr = bsExprFromModel(model);
    bsRelease(model);
    /* GCOV_EXCL_START - the bundled parser cannot produce a model the converter rejects */
    if (expr == NULL) {
        bsParserErrorInternal(error, scriptName, bsParserBootstrap.includeName, "Invalid expression model");
    }
    /* GCOV_EXCL_STOP */
    return expr;
}


BSValue bsLintScript(const BSScript *script, BSValue globals)
{
    BSValue args[2];
    args[0] = bsScriptToModel(script);
    args[1] = globals;
    BSValue warnings = bsParserCall(&bsLintBootstrap, "barescriptLintScript", args, 2, NULL, NULL);
    bsRelease(args[0]);
    /* GCOV_EXCL_START - the bundled linter always returns its warnings array */
    if (warnings.type != BS_ARRAY) {
        bsRelease(warnings);
        return bsArrayNew();
    }
    /* GCOV_EXCL_STOP */
    return warnings;
}
