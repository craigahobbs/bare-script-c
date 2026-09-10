/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The command-line interface unit tests
 *
 * The CLI is exercised by calling bsMain directly with captured standard output and error.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test.h"


/* The captured output of the most recent bsTestMain call */
static BSValue bsTestMainOutput;


/*
 * Run the command-line interface on a NULL-terminated argument list, "bare" implied, with captured
 * stdout and stderr; returns the status code
 */
static int bsTestBare(const char *arg, ...)
{
    /* Copy the arguments into mutable storage - bsMain takes "char **", as main does */
    static char storage[16][1024];
    char *args[16];
    int argc = 0;
    va_list list;
    va_start(list, arg);
    for (const char *text = "bare"; text != NULL; text = argc == 1 ? arg : va_arg(list, const char *)) {
        snprintf(storage[argc], sizeof(storage[argc]), "%s", text);
        args[argc] = storage[argc];
        argc++;
    }
    va_end(list);

    char path[512];
    snprintf(path, sizeof(path), "%s/cli-output.txt", bsTestTempDir());

    fflush(stdout);
    fflush(stderr);
    int savedOut = dup(STDOUT_FILENO);
    int savedErr = dup(STDERR_FILENO);
    FILE *capture = freopen(path, "w+", stdout);
    if (capture == NULL) {
        return -1; /* GCOV_EXCL_LINE */
    }
    dup2(STDOUT_FILENO, STDERR_FILENO);

    int status = bsMain(argc, args);

    fflush(stdout);
    fflush(stderr);
    dup2(savedOut, STDOUT_FILENO);
    dup2(savedErr, STDERR_FILENO);
    close(savedOut);
    close(savedErr);
    clearerr(stdout);

    BSFetchRequest request = {.url = path, .headers = bsNull()};
    BSValue output = bsNull();
    bsFetchReadOnly(&request, &output, 1, NULL);
    bsAssign(&bsTestMainOutput, bsIsType(output, BS_STRING) ? output : bsStringNew(""));
    return status;
}


static const char *bsTestMainText(void)
{
    return bsStringData(bsTestMainOutput);
}


TEST(bare_help)
{
    ASSERT_INT_EQ(bsTestBare("-h", NULL), 0);
    ASSERT_STR_CONTAINS(bsTestMainText(), "usage: bare");

    ASSERT_INT_EQ(bsTestBare("--help", NULL), 0);
    ASSERT_STR_CONTAINS(bsTestMainText(), "usage: bare");

    /* No arguments prints the usage */
    ASSERT_INT_EQ(bsTestBare(NULL), 0);
    ASSERT_STR_CONTAINS(bsTestMainText(), "usage: bare");

    ASSERT_INT_EQ(bsTestBare("--version", NULL), 0);
    ASSERT_STR_CONTAINS(bsTestMainText(), BARESCRIPT_VERSION);
}


TEST(bare_code)
{
    ASSERT_INT_EQ(bsTestBare("-c", "systemLog('hello')", NULL), 0);
    ASSERT_STR_EQ(bsTestMainText(), "hello\n");

    ASSERT_INT_EQ(bsTestBare("--code", "systemLog('long')", NULL), 0);
    ASSERT_STR_EQ(bsTestMainText(), "long\n");

    /* Multiple inline scripts share globals and are numbered */
    ASSERT_INT_EQ(bsTestBare("-c", "x = 1", "-c", "undefinedFunc()", NULL), 1);
    ASSERT_STR_EQ(bsTestMainText(), "<string2>:1: Undefined function \"undefinedFunc\"\n");

    /* A missing argument */
    ASSERT_INT_EQ(bsTestBare("-c", NULL), 2);
    ASSERT_STR_CONTAINS(bsTestMainText(), "expected one argument");
}


TEST(bare_exit_codes)
{
    ASSERT_INT_EQ(bsTestBare("-c", "return 0", NULL), 0);

    ASSERT_INT_EQ(bsTestBare("-c", "return 3", NULL), 3);

    ASSERT_INT_EQ(bsTestBare("-c", "return 256", NULL), 1);

    ASSERT_INT_EQ(bsTestBare("-c", "return true", NULL), 1);

    ASSERT_INT_EQ(bsTestBare("-c", "return false", NULL), 0);

    ASSERT_INT_EQ(bsTestBare("-c", "return null", NULL), 0);

    ASSERT_INT_EQ(bsTestBare("-c", "return 'x'", NULL), 1);

    ASSERT_INT_EQ(bsTestBare("-c", "return -1", NULL), 1);

    ASSERT_INT_EQ(bsTestBare("-c", "return 1.5", NULL), 1);
}


TEST(bare_variables)
{
    ASSERT_INT_EQ(bsTestBare("-v", "vName", "'World'", "-c", "systemLog('Hello, ' + vName)", NULL), 0);
    ASSERT_STR_EQ(bsTestMainText(), "Hello, World\n");

    ASSERT_INT_EQ(bsTestBare("--var", "vNum", "1 + 2", "-c", "systemLog(vNum)", NULL), 0);
    ASSERT_STR_EQ(bsTestMainText(), "3\n");

    /* The built-in expression functions are available */
    ASSERT_INT_EQ(bsTestBare("-v", "vMax", "max(1, 5)", "-c", "systemLog(vMax)", NULL), 0);
    ASSERT_STR_EQ(bsTestMainText(), "5\n");

    /* An invalid expression */
    ASSERT_INT_EQ(bsTestBare("-v", "vBad", "1 +", "-c", "systemLog('never')", NULL), 1);
    ASSERT_STR_CONTAINS(bsTestMainText(), "Syntax error");

    /* An expression error stops the run, under static analysis too */
    ASSERT_INT_EQ(bsTestBare("-s", "-v", "vBad", "nope()", "-c", "return 1", NULL), 1);
    ASSERT_STR_EQ(bsTestMainText(), "Undefined function \"nope\"\n");
    ASSERT_INT_EQ(bsTestBare("-s", "-v", "vBad", "1 +", "-c", "return 1", NULL), 1);
    ASSERT_STR_NOT_CONTAINS(bsTestMainText(), "static analysis");

    /* A missing argument */
    ASSERT_INT_EQ(bsTestBare("-v", "vName", NULL), 2);
    ASSERT_STR_CONTAINS(bsTestMainText(), "expected 2 arguments");
}


TEST(bare_files)
{
    const char *path = bsTestTempFile("cli.bare", "systemLog('from file')\n");
    ASSERT_INT_EQ(bsTestBare(path, NULL), 0);
    ASSERT_STR_EQ(bsTestMainText(), "from file\n");

    /* A missing file */
    ASSERT_INT_EQ(bsTestBare("no-such-file-xyz.bare", NULL), 1);
    ASSERT_STR_CONTAINS(bsTestMainText(), "Failed to load");

    /* A file with a syntax error */
    const char *badPath = bsTestTempFile("bad.bare", "a = 1 +\n");
    ASSERT_INT_EQ(bsTestBare(badPath, NULL), 1);
    ASSERT_STR_CONTAINS(bsTestMainText(), "Syntax error");

    /* An unrecognized option */
    ASSERT_INT_EQ(bsTestBare("--nope", NULL), 2);
    ASSERT_STR_CONTAINS(bsTestMainText(), "unrecognized arguments: --nope");

    /* A bare "-" is a file name */
    ASSERT_INT_EQ(bsTestBare("-", NULL), 1);
    ASSERT_STR_CONTAINS(bsTestMainText(), "Failed to load");
}


TEST(bare_includes)
{
    bsTestTempFile("lib.bare", "function libFn():\n    return 'from lib'\nendfunction\n");
    const char *mainPath = bsTestTempFile("main.bare", "include 'lib.bare'\nsystemLog(libFn())\n");

    ASSERT_INT_EQ(bsTestBare(mainPath, NULL), 0);
    ASSERT_STR_EQ(bsTestMainText(), "from lib\n");
}


TEST(bare_debug_and_static)
{
    /* Debug mode also logs the script execution time */
    ASSERT_INT_EQ(bsTestBare("-d", "-c", "systemLogDebug('debug on')", NULL), 0);
    ASSERT_STR_CONTAINS(bsTestMainText(), "debug on\n");
    ASSERT_STR_CONTAINS(bsTestMainText(), "BareScript executed in");

    ASSERT_INT_EQ(bsTestBare("--debug", "-c", "systemLogDebug('debug long')", NULL), 0);
    ASSERT_STR_CONTAINS(bsTestMainText(), "debug long\n");

    /* The static analysis long option, on a parse failure */
    ASSERT_INT_EQ(bsTestBare("--static", "-c", "a = 1 +", NULL), 1);
    ASSERT_STR_CONTAINS(bsTestMainText(), "Syntax error");
}


TEST(bare_multiple_files)
{
    /* Two file scripts in sequence exercise the URL function replacement between them */
    const char *first = bsTestTempFile("first.bare", "systemLog('first')\n");
    char firstPath[512];
    snprintf(firstPath, sizeof(firstPath), "%s", first);
    const char *second = bsTestTempFile("second.bare", "systemLog('second')\n");

    ASSERT_INT_EQ(bsTestBare(firstPath, second, NULL), 0);
    ASSERT_STR_EQ(bsTestMainText(), "first\nsecond\n");
}


TEST(bare_static_analysis)
{
    /* Static analysis of a clean script */
    ASSERT_INT_EQ(bsTestBare("-s", "-c", "return 1", NULL), 0);
    ASSERT_STR_EQ(bsTestMainText(), "BareScript static analysis \"<string>\" ... OK\n");

    /* A script with one warning */
    ASSERT_INT_EQ(bsTestBare("-s", "-c", "1 + 2", NULL), 1);
    ASSERT_STR_CONTAINS(bsTestMainText(), "... 1 warning:\n");
    ASSERT_STR_CONTAINS(bsTestMainText(), "Pointless global statement");

    /* A script with several warnings */
    ASSERT_INT_EQ(bsTestBare("-s", "-c",
                              "function f():\n    unused = 1\n    return 1\nendfunction\n1 + 2\n", NULL), 1);
    ASSERT_STR_CONTAINS(bsTestMainText(), " warnings:\n");
    ASSERT_STR_CONTAINS(bsTestMainText(), "Unused variable");

    /* Static analysis does not execute */
    ASSERT_INT_EQ(bsTestBare("-s", "-c", "systemLog('never runs')", NULL), 0);
    ASSERT_STR_NOT_CONTAINS(bsTestMainText(), "never runs");

    /* Static analysis with execution */
    ASSERT_INT_EQ(bsTestBare("-x", "-c", "systemLog('runs')", NULL), 0);
    ASSERT_STR_CONTAINS(bsTestMainText(), "runs\n");
    ASSERT_STR_CONTAINS(bsTestMainText(), "... OK\n");

    ASSERT_INT_EQ(bsTestBare("--staticx", "-c", "return 0", NULL), 0);
    ASSERT_STR_CONTAINS(bsTestMainText(), "... OK\n");

    /* A runtime error stops the run before static analysis */
    ASSERT_INT_EQ(bsTestBare("-x", "-c", "undefinedFunc()", NULL), 1);
    ASSERT_STR_CONTAINS(bsTestMainText(), "Undefined function");
    ASSERT_STR_NOT_CONTAINS(bsTestMainText(), "static analysis");

    /* Static analysis continues past a failing script */
    ASSERT_INT_EQ(bsTestBare("-s", "-c", "1 + 2", "-c", "return 1", NULL), 1);
    ASSERT_STR_CONTAINS(bsTestMainText(), "<string2>");
}


TEST(bare_markdownup)
{
    /* MarkdownUp text output wraps the scripts in the markdownUp.bare include */
    ASSERT_INT_EQ(bsTestBare("-m", "-c", "markdownPrint('# Heading')", NULL), 0);
    ASSERT_STR_EQ(bsTestMainText(), "# Heading\n");

    ASSERT_INT_EQ(bsTestBare("--markdown", "-c", "markdownPrint('text')", NULL), 0);
    ASSERT_STR_EQ(bsTestMainText(), "text\n");

    /* MarkdownUp HTML output brackets the scripts with the document begin and end */
    ASSERT_INT_EQ(bsTestBare("-l", "-c", "markdownPrint('# Heading')", NULL), 0);
    ASSERT_STR_CONTAINS(bsTestMainText(), "<!DOCTYPE html>");
    ASSERT_STR_CONTAINS(bsTestMainText(), "</html>");

    ASSERT_INT_EQ(bsTestBare("--html", "-c", "markdownPrint('x')", NULL), 0);
    ASSERT_STR_CONTAINS(bsTestMainText(), "<!DOCTYPE html>");

    /* The MarkdownUp modes set the unittest report globals */
    ASSERT_INT_EQ(bsTestBare("-m", "-c", "systemLog(jsonStringify(vUnittestReport))", NULL), 0);
    ASSERT_STR_CONTAINS(bsTestMainText(), "true");

    ASSERT_INT_EQ(bsTestBare("-m", "-x", "-c",
                                  "systemLog(jsonStringify(vUnittestDisabled))", NULL), 0);
    ASSERT_STR_CONTAINS(bsTestMainText(), "true");

    /* Only the user's own scripts are named, timed, and analyzed */
    ASSERT_INT_EQ(bsTestBare("-m", "-s", "-c", "return 1", "-c", "return 2", NULL), 0);
    ASSERT_STR_CONTAINS(bsTestMainText(), "\"<string>\" ... OK");
    ASSERT_STR_CONTAINS(bsTestMainText(), "\"<string2>\" ... OK");
    ASSERT_STR_NOT_CONTAINS(bsTestMainText(), "markdownUp.bare");

    /* The two MarkdownUp modes are exclusive */
    ASSERT_INT_EQ(bsTestBare("-m", "-l", "-c", "return 1", NULL), 2);
    ASSERT_STR_EQ(bsTestMainText(), "bare: argument -m/--markdown: not allowed with argument -l/--html\n");
}


TEST(bare_status_code_sticky)
{
    /* A later zero result does not clear an earlier non-zero status code */
    ASSERT_INT_EQ(bsTestBare("-x", "-c", "return 3", "-c", "return 0", NULL), 3);
    bsAssign(&bsTestMainOutput, bsNull());
}
