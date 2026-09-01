/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The command-line interface unit tests
 *
 * The CLI is exercised by calling bsMain directly with captured standard output and error.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test.h"


/* The captured output of the most recent bsTestMain call */
static BSValue bsTestMainOutput = {BS_NULL, {0}};


/* Run the command-line interface with captured stdout and stderr; returns the status code */
static int bsTestMain(int argc, const char **argv)
{
    /* Copy the arguments into mutable storage - bsMain takes "char **", as main does */
    static char storage[16][1024];
    char *args[16];
    for (int ix = 0; ix < argc; ix++) {
        snprintf(storage[ix], sizeof(storage[ix]), "%s", argv[ix]);
        args[ix] = storage[ix];
    }

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

    BSFetchRequest request;
    memset(&request, 0, sizeof(request));
    request.url = path;
    request.headers = bsNull();
    size_t size = 0;
    char *text = bsFetchReadOnly(&request, &size, NULL);
    bsAssign(&bsTestMainOutput, text != NULL ? bsStringNewSize(text, size) : bsStringNew(""));
    free(text);
    return status;
}


static const char *bsTestMainText(void)
{
    return bsStringData(bsTestMainOutput);
}


TEST(bare_help)
{
    const char *argvHelp[] = {"bare", "-h"};
    ASSERT_INT_EQ(bsTestMain(2, argvHelp), 0);
    ASSERT_TRUE(strstr(bsTestMainText(), "usage: bare") != NULL);

    const char *argvLongHelp[] = {"bare", "--help"};
    ASSERT_INT_EQ(bsTestMain(2, argvLongHelp), 0);
    ASSERT_TRUE(strstr(bsTestMainText(), "usage: bare") != NULL);

    /* No arguments prints the usage */
    const char *argvNone[] = {"bare"};
    ASSERT_INT_EQ(bsTestMain(1, argvNone), 0);
    ASSERT_TRUE(strstr(bsTestMainText(), "usage: bare") != NULL);

    const char *argvVersion[] = {"bare", "--version"};
    ASSERT_INT_EQ(bsTestMain(2, argvVersion), 0);
    ASSERT_TRUE(strstr(bsTestMainText(), BARESCRIPT_VERSION) != NULL);
}


TEST(bare_code)
{
    const char *argv[] = {"bare", "-c", "systemLog('hello')"};
    ASSERT_INT_EQ(bsTestMain(3, argv), 0);
    ASSERT_STR_EQ(bsTestMainText(), "hello\n");

    const char *argvLong[] = {"bare", "--code", "systemLog('long')"};
    ASSERT_INT_EQ(bsTestMain(3, argvLong), 0);
    ASSERT_STR_EQ(bsTestMainText(), "long\n");

    /* Multiple inline scripts share globals and are numbered */
    const char *argvTwo[] = {"bare", "-c", "x = 1", "-c", "undefinedFunc()"};
    ASSERT_INT_EQ(bsTestMain(5, argvTwo), 1);
    ASSERT_STR_EQ(bsTestMainText(), "<string2>:1: Undefined function \"undefinedFunc\"\n");

    /* A missing argument */
    const char *argvMissing[] = {"bare", "-c"};
    ASSERT_INT_EQ(bsTestMain(2, argvMissing), 2);
    ASSERT_TRUE(strstr(bsTestMainText(), "expected one argument") != NULL);
}


TEST(bare_exit_codes)
{
    const char *argvZero[] = {"bare", "-c", "return 0"};
    ASSERT_INT_EQ(bsTestMain(3, argvZero), 0);

    const char *argvThree[] = {"bare", "-c", "return 3"};
    ASSERT_INT_EQ(bsTestMain(3, argvThree), 3);

    const char *argvBig[] = {"bare", "-c", "return 256"};
    ASSERT_INT_EQ(bsTestMain(3, argvBig), 1);

    const char *argvTrue[] = {"bare", "-c", "return true"};
    ASSERT_INT_EQ(bsTestMain(3, argvTrue), 1);

    const char *argvFalse[] = {"bare", "-c", "return false"};
    ASSERT_INT_EQ(bsTestMain(3, argvFalse), 0);

    const char *argvNull[] = {"bare", "-c", "return null"};
    ASSERT_INT_EQ(bsTestMain(3, argvNull), 0);

    const char *argvString[] = {"bare", "-c", "return 'x'"};
    ASSERT_INT_EQ(bsTestMain(3, argvString), 1);

    const char *argvNegative[] = {"bare", "-c", "return -1"};
    ASSERT_INT_EQ(bsTestMain(3, argvNegative), 1);

    const char *argvFraction[] = {"bare", "-c", "return 1.5"};
    ASSERT_INT_EQ(bsTestMain(3, argvFraction), 1);
}


TEST(bare_variables)
{
    const char *argv[] = {"bare", "-v", "vName", "'World'", "-c", "systemLog('Hello, ' + vName)"};
    ASSERT_INT_EQ(bsTestMain(6, argv), 0);
    ASSERT_STR_EQ(bsTestMainText(), "Hello, World\n");

    const char *argvLong[] = {"bare", "--var", "vNum", "1 + 2", "-c", "systemLog(vNum)"};
    ASSERT_INT_EQ(bsTestMain(6, argvLong), 0);
    ASSERT_STR_EQ(bsTestMainText(), "3\n");

    /* The built-in expression functions are available */
    const char *argvBuiltin[] = {"bare", "-v", "vMax", "max(1, 5)", "-c", "systemLog(vMax)"};
    ASSERT_INT_EQ(bsTestMain(6, argvBuiltin), 0);
    ASSERT_STR_EQ(bsTestMainText(), "5\n");

    /* An invalid expression */
    const char *argvBad[] = {"bare", "-v", "vBad", "1 +", "-c", "systemLog('never')"};
    ASSERT_INT_EQ(bsTestMain(6, argvBad), 1);
    ASSERT_TRUE(strstr(bsTestMainText(), "Syntax error") != NULL);

    /* A missing argument */
    const char *argvMissing[] = {"bare", "-v", "vName"};
    ASSERT_INT_EQ(bsTestMain(3, argvMissing), 2);
    ASSERT_TRUE(strstr(bsTestMainText(), "expected two arguments") != NULL);
}


TEST(bare_files)
{
    const char *path = bsTestTempFile("cli.bare", "systemLog('from file')\n");
    const char *argv[] = {"bare", path};
    ASSERT_INT_EQ(bsTestMain(2, argv), 0);
    ASSERT_STR_EQ(bsTestMainText(), "from file\n");

    /* A missing file */
    const char *argvMissing[] = {"bare", "no-such-file-xyz.bare"};
    ASSERT_INT_EQ(bsTestMain(2, argvMissing), 1);
    ASSERT_TRUE(strstr(bsTestMainText(), "failed to load") != NULL);

    /* A file with a syntax error */
    const char *badPath = bsTestTempFile("bad.bare", "a = 1 +\n");
    const char *argvBad[] = {"bare", badPath};
    ASSERT_INT_EQ(bsTestMain(2, argvBad), 1);
    ASSERT_TRUE(strstr(bsTestMainText(), "Syntax error") != NULL);

    /* An unrecognized option */
    const char *argvUnknown[] = {"bare", "--nope"};
    ASSERT_INT_EQ(bsTestMain(2, argvUnknown), 2);
    ASSERT_TRUE(strstr(bsTestMainText(), "unrecognized argument") != NULL);

    /* A bare "-" is a file name */
    const char *argvDash[] = {"bare", "-"};
    ASSERT_INT_EQ(bsTestMain(2, argvDash), 1);
    ASSERT_TRUE(strstr(bsTestMainText(), "failed to load") != NULL);
}


TEST(bare_includes)
{
    bsTestTempFile("lib.bare", "function libFn():\n    return 'from lib'\nendfunction\n");
    char mainPath[512];
    snprintf(mainPath, sizeof(mainPath), "%s/main.bare", bsTestTempDir());
    FILE *file = fopen(mainPath, "w");
    ASSERT_NOT_NULL(file);
    fputs("include 'lib.bare'\nsystemLog(libFn())\n", file);
    fclose(file);

    const char *argv[] = {"bare", mainPath};
    ASSERT_INT_EQ(bsTestMain(2, argv), 0);
    ASSERT_STR_EQ(bsTestMainText(), "from lib\n");
}


TEST(bare_debug_and_static)
{
    const char *argvDebug[] = {"bare", "-d", "-c", "systemLogDebug('debug on')"};
    ASSERT_INT_EQ(bsTestMain(4, argvDebug), 0);
    ASSERT_STR_EQ(bsTestMainText(), "debug on\n");

    const char *argvDebugLong[] = {"bare", "--debug", "-c", "systemLogDebug('debug long')"};
    ASSERT_INT_EQ(bsTestMain(4, argvDebugLong), 0);
    ASSERT_STR_EQ(bsTestMainText(), "debug long\n");

    /* Static analysis parses without executing */
    const char *argvStatic[] = {"bare", "-s", "-c", "systemLog('never runs')"};
    ASSERT_INT_EQ(bsTestMain(4, argvStatic), 0);
    ASSERT_STR_EQ(bsTestMainText(), "BareScript static analysis \"<string>\" ... OK\n");

    const char *argvStaticLong[] = {"bare", "--static", "-c", "a = 1 +"};
    ASSERT_INT_EQ(bsTestMain(4, argvStaticLong), 1);
    ASSERT_TRUE(strstr(bsTestMainText(), "Syntax error") != NULL);
}


TEST(bare_include_path)
{
    /* The BARESCRIPT_INCLUDE_PATH environment variable adds system include directories */
    bsTestTempFile("sys.bare", "function sysFn():\n    return 'from system'\nendfunction\n");
    BSValue path = bsStringNewFormat(":nonexistent-dir:%s", bsTestTempDir());
    setenv("BARESCRIPT_INCLUDE_PATH", bsStringData(path), 1);
    bsRelease(path);

    const char *argv[] = {"bare", "-c", "include <sys.bare>\nsystemLog(sysFn())"};
    ASSERT_INT_EQ(bsTestMain(3, argv), 0);
    ASSERT_STR_EQ(bsTestMainText(), "from system\n");

    setenv("BARESCRIPT_INCLUDE_PATH", "", 1);
    const char *argvEmpty[] = {"bare", "-c", "systemLog('ok')"};
    ASSERT_INT_EQ(bsTestMain(3, argvEmpty), 0);

    unsetenv("BARESCRIPT_INCLUDE_PATH");
    ASSERT_INT_EQ(bsTestMain(3, argvEmpty), 0);

    bsAssign(&bsTestMainOutput, bsNull());
}


TEST(bare_multiple_files)
{
    /* Two file scripts in sequence exercise the URL function replacement between them */
    const char *first = bsTestTempFile("first.bare", "systemLog('first')\n");
    char firstPath[512];
    snprintf(firstPath, sizeof(firstPath), "%s", first);
    const char *second = bsTestTempFile("second.bare", "systemLog('second')\n");

    const char *argv[] = {"bare", firstPath, second};
    ASSERT_INT_EQ(bsTestMain(3, argv), 0);
    ASSERT_STR_EQ(bsTestMainText(), "first\nsecond\n");
    bsAssign(&bsTestMainOutput, bsNull());
}
