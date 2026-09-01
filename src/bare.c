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

#include "internal.h"


const char *bsVersion(void)
{
    return BARESCRIPT_VERSION;
}


static const char *bsUsage =
    "usage: bare [-h] [-c CODE] [-d] [-s] [-x] [-v VAR EXPR] [--version] [file ...]\n"
    "\n"
    "The BareScript command-line interface\n"
    "\n"
    "positional arguments:\n"
    "  file           files to process\n"
    "\n"
    "options:\n"
    "  -h, --help     show this help message and exit\n"
    "  -c, --code     execute the BareScript code\n"
    "  -d, --debug    enable debug mode\n"
    "  -s, --static   perform static analysis\n"
    "  -x, --staticx  perform static analysis with execution\n"
    "  -v, --var      set a global variable to an expression value\n"
    "  --version      show the version and exit\n";


/* A command-line script source - a file path or inline code */
typedef struct BSScriptSource {
    bool isFile;
    const char *value;
} BSScriptSource;


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
    BSScriptSource *sources = bsAlloc((size_t) (argc > 0 ? argc : 1) * sizeof(BSScriptSource));
    size_t sourceCount = 0;
    const char **varNames = bsAlloc((size_t) (argc > 0 ? argc : 1) * sizeof(char *));
    const char **varExprs = bsAlloc((size_t) (argc > 0 ? argc : 1) * sizeof(char *));
    size_t varCount = 0;
    bool debug = false;
    bool staticAnalysis = false;
    bool staticExecute = false;
    int statusCode = 0;

    /* Parse the command-line arguments */
    for (int ix = 1; ix < argc; ix++) {
        const char *arg = argv[ix];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            fputs(bsUsage, stdout);
            free(sources);
            free(varNames);
            free(varExprs);
            return 0;
        }
        if (strcmp(arg, "--version") == 0) {
            printf("%s\n", bsVersion());
            free(sources);
            free(varNames);
            free(varExprs);
            return 0;
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
        if (strcmp(arg, "-c") == 0 || strcmp(arg, "--code") == 0) {
            if (ix + 1 >= argc) {
                bsPrintError("bare: argument -c/--code: expected one argument");
                statusCode = 2;
                break;
            }
            sources[sourceCount].isFile = false;
            sources[sourceCount].value = argv[++ix];
            sourceCount++;
            continue;
        }
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--var") == 0) {
            if (ix + 2 >= argc) {
                bsPrintError("bare: argument -v/--var: expected two arguments");
                statusCode = 2;
                break;
            }
            varNames[varCount] = argv[++ix];
            varExprs[varCount] = argv[++ix];
            varCount++;
            continue;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "bare: unrecognized argument: %s\n", arg);
            statusCode = 2;
            break;
        }
        sources[sourceCount].isFile = true;
        sources[sourceCount].value = arg;
        sourceCount++;
    }

    if (statusCode == 0 && sourceCount == 0) {
        fputs(bsUsage, stdout);
    } else if (statusCode == 0) {
        /* Add the BARESCRIPT_INCLUDE_PATH directories to the system include search path */
        const char *includePath = getenv("BARESCRIPT_INCLUDE_PATH");
        if (includePath != NULL) {
            char *paths = bsStrdup(includePath);
            char *begin = paths;
            while (*begin != '\0') {
                char *end = strchr(begin, ':');
                if (end != NULL) {
                    *end = '\0';
                }
                if (*begin != '\0') {
                    bsSystemIncludePath(begin);
                }
                if (end == NULL) {
                    break;
                }
                begin = end + 1;
            }
            free(paths);
        }

        BSOptions *options = bsOptionsNew();
        options->debug = debug;
        options->fetchFn = bsFetchReadWrite;
        options->logFn = bsLogStdout;

        /* Evaluate the global variable expression arguments */
        for (size_t ix = 0; ix < varCount && statusCode == 0; ix++) {
            BSParserError parserError;
            memset(&parserError, 0, sizeof(parserError));
            BSExpr *expr = bsParseExpression(varExprs[ix], strlen(varExprs[ix]), 0, NULL, false, &parserError);
            if (expr == NULL) {
                bsPrintError(bsStringData(parserError.message));
                bsParserErrorFree(&parserError);
                statusCode = 1;
                break;
            }
            BSValue value = bsEvaluateExpression(expr, options, NULL, true);
            bsExprFree(expr);
            bsObjectSet(options->globals, varNames[ix], value);
        }

        /* Parse and execute each script source in order */
        size_t inlineCount = 0;
        for (size_t ix = 0; ix < sourceCount && (statusCode == 0 || staticAnalysis); ix++) {
            char *text = NULL;
            size_t size = 0;
            char scriptNameBuffer[32];
            const char *scriptName;
            if (sources[ix].isFile) {
                scriptName = sources[ix].value;
                BSFetchRequest request;
                memset(&request, 0, sizeof(request));
                request.url = sources[ix].value;
                request.headers = bsNull();
                text = bsFetchReadWrite(&request, &size, NULL);
                if (text == NULL) {
                    fprintf(stderr, "bare: failed to load \"%s\"\n", sources[ix].value);
                    statusCode = 1;
                    break;
                }
            } else {
                inlineCount++;
                if (inlineCount > 1) {
                    snprintf(scriptNameBuffer, sizeof(scriptNameBuffer), "<string%zu>", inlineCount);
                } else {
                    snprintf(scriptNameBuffer, sizeof(scriptNameBuffer), "<string>");
                }
                scriptName = scriptNameBuffer;
                size = strlen(sources[ix].value);
                text = bsAlloc(size + 1);
                memcpy(text, sources[ix].value, size + 1);
            }

            /* Parse the script source */
            BSParserError parserError;
            memset(&parserError, 0, sizeof(parserError));
            BSScript *script = bsParseScript(text, size, 1, scriptName, &parserError);
            free(text);
            if (script == NULL) {
                bsPrintError(bsStringData(parserError.message));
                bsParserErrorFree(&parserError);
                statusCode = 1;
                break;
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
            if (!staticAnalysis || staticExecute) {
                char *scriptPath = sources[ix].isFile ? bsStrdup(sources[ix].value) : NULL;
                if (options->urlDataFree != NULL) {
                    options->urlDataFree(options->urlData);
                }
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
                } else if (result.type == BS_NUMBER && trunc(result.u.number) == result.u.number &&
                           result.u.number >= 0 && result.u.number <= 255) {
                    statusCode = (int) result.u.number;
                } else if (bsValueBoolean(result)) {
                    statusCode = 1;
                }
                bsRelease(result);

                /* Log the script execution time in debug mode */
                if (debug) {
                    printf("BareScript executed in %.1f milliseconds\n",
                           (double) (bsDatetimeNow() - timeBegin));
                }
            }

            /* Run the linter - a runtime error stops the run before static analysis */
            if (staticAnalysis && !runtimeFailed) {
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

        bsOptionsFree(options);
    }

    free(sources);
    free(varNames);
    free(varExprs);
    bsParserCleanup();
    bsSystemIncludeClear();
    bsIncludeCleanup();
    bsLibraryCleanup();
    return statusCode;
}
