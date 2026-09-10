/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The BareScript parser and linter
 *
 * BareScript is parsed by barescriptParser.bare and linted by barescriptLint.bare, the same
 * include library scripts the JavaScript and Python implementations use, running on this runtime.
 * Both are bootstrapped straight from their bundled, parser-compiled binary script models - there is
 * no parser to run before the parser exists - and their output models are converted to the
 * runtime's compiled representation by model.c.
 */

#include <string.h>

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
} BSBootstrap;

static _Thread_local BSBootstrap bsParserBootstrap = {"barescriptParser.bare", NULL};
static _Thread_local BSBootstrap bsLintBootstrap = {"barescriptLint.bare", NULL};


/* Load a bundled include library script and execute it, returning its globals */
static BSValue bsBootstrapGlobals(BSBootstrap *bootstrap)
{
    if (bootstrap->options != NULL) {
        return bootstrap->options->globals;
    }

    /*
     * The bundled model loads without a parser - which is what makes it possible for the parser
     * itself to be a BareScript script. The compiled script is cached.
     */
    BSScript *script = bsIncludeScript(bootstrap->includeName);
    if (script == NULL) {
        return bsNull(); /* GCOV_EXCL_LINE - parser and linter models always load */
    }

    BSOptions *options = bsOptionsNew();
    bsRelease(bsExecuteScript(script, options));
    bsScriptRelease(script);

    bootstrap->options = options;
    return options->globals;
}


void bsParserCleanup(void)
{
    bsOptionsFree(bsParserBootstrap.options);
    bsParserBootstrap.options = NULL;
    bsOptionsFree(bsLintBootstrap.options);
    bsLintBootstrap.options = NULL;
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
    error->columnNumber = bsIsNumber(columnNumber) ? (int) bsNumberOf(columnNumber) : 0;
    error->lineNumber = bsIsNumber(lineNumber) ? (int) bsNumberOf(lineNumber) : 0;
    error->message = bsRetain(bsObjectGet(errorModel, "message"));
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
    BSValue function = bsObjectGet(bsBootstrapGlobals(bootstrap), functionName);
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
    if (!bsIsType(result, BS_OBJECT)) {
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


/*
 * Parse text with a parser function whose arguments are the text, its line, its script name, and
 * "extra" when argCount says so. Returns the owned model, or a null value.
 */
static BSValue bsParserParse(const char *functionName, BSValue text, BSValue line, const char *scriptName,
                             BSValue extra, size_t argCount, BSParserError *error)
{
    BSValue args[4] = {text, line, scriptName != NULL ? bsStringNew(scriptName) : bsNull(), extra};
    BSValue result = bsParserCall(&bsParserBootstrap, functionName, args, argCount, scriptName, error);
    bsRelease(args[2]);
    return bsParserUnwrap(result, error);
}


BSScript *bsParseScript(const char *text, size_t size, int startLineNumber, const char *scriptName,
                        BSParserError *error)
{
    BSValue string = bsStringNewSize(text, size);
    BSScript *script = bsParseScriptString(string, startLineNumber, scriptName, error);
    bsRelease(string);
    return script;
}


BSScript *bsParseScriptString(BSValue text, int startLineNumber, const char *scriptName, BSParserError *error)
{
    BSValue model = bsParserParse("barescriptParseScriptEx", text, bsNumber(startLineNumber), scriptName,
                                  bsNull(), 3, error);
    if (!bsIsType(model, BS_OBJECT)) {
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
    BSValue args[3] = {script->scriptLines, bsNumber(script->startLineNumber), script->scriptName};
    BSValue result = bsParserCall(&bsParserBootstrap, "barescriptParseScriptEx", args, 3, NULL, NULL);
    return bsParserUnwrap(result, NULL);
}


BSExpr *bsParseExpression(const char *text, size_t size, int lineNumber, const char *scriptName,
                          bool arrayLiterals, BSParserError *error)
{
    BSValue string = bsStringNewSize(text, size);
    BSValue model = bsParserParse("barescriptParseExpressionEx", string,
                                  lineNumber != 0 ? bsNumber(lineNumber) : bsNull(), scriptName,
                                  bsBoolean(arrayLiterals), 4, error);
    bsRelease(string);
    if (!bsIsType(model, BS_OBJECT)) {
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
    BSValue args[2] = {bsScriptToModel(script), globals};
    BSValue warnings = bsParserCall(&bsLintBootstrap, "barescriptLintScript", args, 2, NULL, NULL);
    bsRelease(args[0]);
    /* GCOV_EXCL_START - the bundled linter always returns its warnings array */
    if (!bsIsType(warnings, BS_ARRAY)) {
        bsRelease(warnings);
        return bsArrayNew();
    }
    /* GCOV_EXCL_STOP */
    return warnings;
}
