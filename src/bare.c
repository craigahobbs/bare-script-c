/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The BareScript command-line interface
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "barescript/barescript.h"


static void *bsCliAlloc(size_t size)
{
    void *ptr = malloc(size);
    if (ptr == NULL) { /* GCOV_EXCL_START */
        fprintf(stderr, "out of memory\n");
        abort();
    } /* GCOV_EXCL_STOP */
    return ptr;
}

static char *bsCliStrdup(const char *text)
{
    size_t size = strlen(text) + 1;
    char *copy = bsCliAlloc(size);
    memcpy(copy, text, size);
    return copy;
}


static const char *bsUsage =
    "usage: bare [-h] [-c CODE] [-d] [-l | -m] [-s] [-x] [-v VAR EXPR] [--version] [file ...]\n"
    "\n"
    "The BareScript command-line interface\n"
    "\n"
    "positional arguments:\n"
    "  file            files to process\n"
    "\n"
    "options:\n"
    "  -h, --help      show this help message and exit\n"
    "  -c, --code      execute the BareScript code\n"
    "  -d, --debug     enable debug mode\n"
    "  -l, --html      run with MarkdownUp HTML output\n"
    "  -m, --markdown  run with MarkdownUp text output\n"
    "  -s, --static    perform static analysis\n"
    "  -x, --staticx   perform static analysis with execution\n"
    "  -v, --var       set a global variable to an expression value\n"
    "  --version       show the version and exit\n";


/* A command-line script source - a file path or inline code */
typedef struct BSScriptSource {
    bool isFile;
    const char *value;
} BSScriptSource;


/* A "-v" argument - a global variable name and the expression that sets it */
typedef struct BSVarArg {
    const char *name;
    const char *expr;
} BSVarArg;


static void bsPrintError(const char *text)
{
    /* Runtime and parser error messages carry their own trailing newline handling */
    size_t size = strlen(text);
    while (size != 0 && (text[size - 1] == '\n' || text[size - 1] == ' ')) {
        size--;
    }
    fprintf(stderr, "%.*s\n", (int) size, text);
}


int bsMain(int argc, char **argv)
{
    /* Three extra slots for the MarkdownUp preamble and postamble */
    BSScriptSource *sources = bsCliAlloc((size_t) (argc + 3) * sizeof(BSScriptSource));
    size_t sourceCount = 0;
    BSVarArg *vars = bsCliAlloc((size_t) (argc + 1) * sizeof(BSVarArg));
    size_t varCount = 0;
    bool debug = false;
    bool staticAnalysis = false;
    bool staticExecute = false;
    bool markdown = false;
    bool html = false;
    int statusCode = 0;

    /* Parse the command-line arguments */
    for (int ix = 1; ix < argc; ix++) {
        const char *arg = argv[ix];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            fputs(bsUsage, stdout);
            goto done;
        }
        if (strcmp(arg, "--version") == 0) {
            printf("%s\n", bsVersion());
            goto done;
        }
        if (strcmp(arg, "-d") == 0 || strcmp(arg, "--debug") == 0) {
            debug = true;
            continue;
        }
        if (strcmp(arg, "-s") == 0 || strcmp(arg, "--static") == 0) {
            staticAnalysis = true;
            staticExecute = false;
            continue;
        }
        if (strcmp(arg, "-x") == 0 || strcmp(arg, "--staticx") == 0) {
            staticAnalysis = true;
            staticExecute = true;
            continue;
        }
        if (strcmp(arg, "-m") == 0 || strcmp(arg, "--markdown") == 0) {
            markdown = true;
            continue;
        }
        if (strcmp(arg, "-l") == 0 || strcmp(arg, "--html") == 0) {
            html = true;
            continue;
        }
        if (strcmp(arg, "-c") == 0 || strcmp(arg, "--code") == 0) {
            if (ix + 1 >= argc) {
                bsPrintError("bare: argument -c/--code: expected one argument");
                statusCode = 2;
                break;
            }
            sources[sourceCount++] = (BSScriptSource) {false, argv[++ix]};
            continue;
        }
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--var") == 0) {
            if (ix + 2 >= argc) {
                bsPrintError("bare: argument -v/--var: expected 2 arguments");
                statusCode = 2;
                break;
            }
            vars[varCount++] = (BSVarArg) {argv[ix + 1], argv[ix + 2]};
            ix += 2;
            continue;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "bare: unrecognized arguments: %s\n", arg);
            statusCode = 2;
            break;
        }
        sources[sourceCount++] = (BSScriptSource) {true, arg};
    }
    if (statusCode == 0 && markdown && html) {
        bsPrintError("bare: argument -m/--markdown: not allowed with argument -l/--html");
        statusCode = 2;
    }
    bool markdownUp = markdown || html;

    /*
     * The MarkdownUp modes wrap the user's scripts in the markdownUp.bare include and, for HTML
     * output, its document begin and end calls. "ixUserScript" is where the user's own scripts
     * start, which is what inline script naming, debug timing, and static analysis key off.
     */
    size_t ixUserScript = 0;
    if (statusCode == 0 && sourceCount != 0 && markdownUp) {
        size_t extra = html ? 2 : 1;
        memmove(sources + extra, sources, sourceCount * sizeof(BSScriptSource));
        sourceCount += extra;
        sources[0] = (BSScriptSource) {false, "include <markdownUp.bare>"};
        if (html) {
            sources[1] = (BSScriptSource) {false, "markdownUpHTMLBegin()"};
            sources[sourceCount++] = (BSScriptSource) {false, "markdownUpHTMLEnd()"};
        }
        ixUserScript = extra;
    }

    if (statusCode == 0 && sourceCount == 0) {
        fputs(bsUsage, stdout);
    } else if (statusCode == 0) {
        /* Add the BARESCRIPT_INCLUDE_PATH directories to the system include search path */
        const char *includePath = getenv("BARESCRIPT_INCLUDE_PATH");
        if (includePath != NULL) {
            char *paths = bsCliStrdup(includePath);
            char *state;
            for (char *path = strtok_r(paths, ":", &state); path != NULL;
                 path = strtok_r(NULL, ":", &state)) {
                bsSystemIncludePath(path);
            }
            free(paths);
        }

        BSOptions *options = bsOptionsNew();
        options->debug = debug;
        options->fetchFn = bsFetchReadWrite;
        options->logFn = bsLogStdout;

        /* The shared globals, which each script executes against unless static analysis isolates it */
        BSValue sharedGlobals = bsRetain(options->globals);
        if (markdownUp) {
            bsObjectSet(sharedGlobals, "vUnittestReport", bsBoolean(true));
            if (staticAnalysis) {
                bsObjectSet(sharedGlobals, "vUnittestDisabled", bsBoolean(true));
            }
        }

        /* Evaluate the global variable expression arguments */
        for (size_t ix = 0; ix < varCount && statusCode == 0; ix++) {
            BSParserError parserError = {0};
            const char *exprText = vars[ix].expr;
            BSExpr *expr = bsParseExpression(exprText, strlen(exprText), 0, NULL, false, &parserError);
            if (expr == NULL) {
                bsPrintError(bsStringData(parserError.message));
                bsParserErrorFree(&parserError);
                statusCode = 1;
                break;
            }
            BSValue value = bsEvaluateExpression(expr, options, NULL, true);
            bsExprFree(expr);
            bsObjectSet(sharedGlobals, vars[ix].name, value);
            const char *error = bsErrorGet(options);
            if (error != NULL) {
                bsPrintError(error);
                bsErrorClear(options);
                statusCode = 1;
            }
        }
        bool varFailed = statusCode != 0;

        /* Parse and execute each script source in order */
        size_t inlineCount = 0;
        for (size_t ix = 0; ix < sourceCount && !varFailed && (statusCode == 0 || staticAnalysis); ix++) {
            BSValue text = bsNull();
            char scriptNameBuffer[32];
            const char *scriptName;
            if (sources[ix].isFile) {
                scriptName = sources[ix].value;
                BSFetchRequest request = {.url = sources[ix].value, .headers = bsNull()};
                bsFetchReadWrite(&request, &text, 1, NULL);
                if (text.type != BS_STRING) {
                    fprintf(stderr, "Failed to load \"%s\"\n", sources[ix].value);
                    statusCode = 1;
                    break;
                }
            } else {
                inlineCount++;
                size_t inlineDisplay = inlineCount > ixUserScript ? inlineCount - ixUserScript : 0;
                if (inlineDisplay > 1) {
                    snprintf(scriptNameBuffer, sizeof(scriptNameBuffer), "<string%zu>", inlineDisplay);
                } else {
                    snprintf(scriptNameBuffer, sizeof(scriptNameBuffer), "<string>");
                }
                scriptName = scriptNameBuffer;
                text = bsStringNew(sources[ix].value);
            }

            /* Parse the script source */
            BSParserError parserError = {0};
            BSScript *script = bsParseScriptString(text, 1, scriptName, &parserError);
            bsRelease(text);
            if (script == NULL) {
                bsPrintError(bsStringData(parserError.message));
                bsParserErrorFree(&parserError);
                statusCode = 1;
                break;
            }
            if (!staticAnalysis) {
                bsScriptForgetModel(script);
            }

            /*
             * Execute the script
             *
             * Static analysis without execution ("-s") lints the parsed script alone; static
             * analysis with execution ("-x") executes first, so the linter sees the globals the
             * script defined.
             */
            BSValue staticGlobals = bsNull();
            bool runtimeFailed = false;
            bool isUserScript = (ix >= ixUserScript);
            if (!staticAnalysis || staticExecute) {
                /*
                 * Under static analysis each user script executes against its own copy of the
                 * globals, so one script's definitions do not leak into the next one's analysis
                 */
                if (staticAnalysis && isUserScript) {
                    BSValue isolated = bsObjectCopy(sharedGlobals);
                    BSValue includes = bsObjectGet(isolated, BS_GLOBAL_INCLUDES);
                    if (includes.type == BS_OBJECT) {
                        bsObjectSet(isolated, BS_GLOBAL_INCLUDES, bsObjectCopy(includes));
                    }
                    bsAssign(&options->globals, isolated);
                } else {
                    bsAssign(&options->globals, bsRetain(sharedGlobals));
                }

                char *scriptPath = sources[ix].isFile ? bsCliStrdup(sources[ix].value) : NULL;
                free(options->urlData);
                options->urlFn = scriptPath != NULL ? bsUrlFileRelative : NULL;
                options->urlData = scriptPath;
                options->urlDataFree = scriptPath != NULL ? free : NULL;

                int64_t timeBegin = bsDatetimeNow();
                BSValue result = bsExecuteScript(script, options);
                staticGlobals = options->globals;
                const char *error = bsErrorGet(options);
                if (error != NULL) {
                    bsPrintError(error);
                    bsErrorClear(options);
                    statusCode = 1;
                    runtimeFailed = true;
                } else {
                    /* A zero result never clears a status code an earlier script set */
                    int resultStatus;
                    if (result.type == BS_NUMBER && trunc(result.u.number) == result.u.number &&
                        result.u.number >= 0 && result.u.number <= 255) {
                        resultStatus = (int) result.u.number;
                    } else {
                        resultStatus = bsValueBoolean(result) ? 1 : 0;
                    }
                    if (resultStatus != 0) {
                        statusCode = resultStatus;
                    }
                }
                bsRelease(result);

                /* Log the script execution time in debug mode */
                if (debug && isUserScript) {
                    printf("BareScript executed in %.1f milliseconds\n",
                           (double) (bsDatetimeNow() - timeBegin));
                }
            }

            /* Run the linter - a runtime error stops the run before static analysis */
            if (staticAnalysis && isUserScript && !runtimeFailed) {
                BSValue warnings = bsLintScript(script, staticGlobals);
                size_t warningCount = bsArrayCount(warnings);
                if (warningCount == 0) {
                    printf("BareScript static analysis \"%s\" ... OK\n", scriptName);
                } else {
                    printf("BareScript static analysis \"%s\" ... %zu warning%s:\n", scriptName,
                           warningCount, warningCount > 1 ? "s" : "");
                    for (size_t ixWarning = 0; ixWarning < warningCount; ixWarning++) {
                        BSValue warning = bsValueString(bsArrayGet(warnings, ixWarning));
                        printf("%s\n", bsStringData(warning));
                        bsRelease(warning);
                    }
                    statusCode = 1;
                }
                bsRelease(warnings);
            }
            bsScriptRelease(script);
            if (runtimeFailed) {
                break;
            }
        }

        bsRelease(sharedGlobals);
        bsOptionsFree(options);
    }

done:
    free(sources);
    free(vars);
    bsParserCleanup();
    bsSystemIncludeClear();
    bsIncludeCleanup();
    bsLibraryCleanup();
    bsValueCleanup();
    return statusCode;
}
