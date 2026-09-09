/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The unit test framework
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "test.h"


/* The registered test cases, in registration order */
static BSTestCase *bsTestHead = NULL;
static BSTestCase *bsTestTail = NULL;

/* The current test's state */
static jmp_buf bsTestJump;
static const BSTestCase *bsTestCurrent = NULL;
static bool bsTestFailed = false;
static size_t bsTestAssertions = 0;

/* Quiet mode prints a dot per test and collects the failure details for the end */
static bool bsTestQuiet = false;
static BSStringBuilder bsTestFailText;


void bsTestRegister(BSTestCase *testCase)
{
    if (bsTestHead == NULL) {
        bsTestHead = testCase;
    } else {
        bsTestTail->next = testCase;
    }
    bsTestTail = testCase;
}


void bsTestPass(void)
{
    bsTestAssertions++;
}


void bsTestFail(const char *file, int line, const char *format, ...)
{
    bsTestAssertions++;
    bsTestFailed = true;

    /* Format the failure message */
    va_list args;
    va_start(args, format);
    BSValue message = bsStringNewVFormat(format, args);
    va_end(args);

    /* Report it now, under the test's line, or collect it for the end of a quiet run */
    if (bsTestQuiet) {
        putchar('F');
        fflush(stdout);
        bsSBAppendFormat(&bsTestFailText,
                         "======================================================================\n"
                         "FAIL: %s\n    %s:%d: %s\n", bsTestCurrent->name, file, line, bsStringData(message));
    } else {
        printf("FAIL\n    %s:%d: %s\n", file, line, bsStringData(message));
    }
    bsRelease(message);
    longjmp(bsTestJump, 1);
}


void bsTestCheck(const char *file, int line, bool ok, const char *message)
{
    if (!ok) {
        bsTestFail(file, line, "%s", message);
    } else {
        bsTestPass();
    }
}


void bsTestAssertContains(const char *file, int line, const char *subject, const char *haystack, const char *needle,
                          bool expected)
{
    if ((strstr(haystack, needle) != NULL) != expected) {
        bsTestFail(file, line, "%s - actual \"%s\"", subject, haystack);
    } else {
        bsTestPass();
    }
}


void bsTestAssertEqual(const char *file, int line, const char *subject, const char *actual, const char *expected)
{
    bool equal = actual == NULL || expected == NULL ? actual == expected : strcmp(actual, expected) == 0;
    if (!equal) {
        bsTestFail(file, line, "%s\n    actual:   %s\n    expected: %s", subject, actual != NULL ? actual : "(null)",
                   expected != NULL ? expected : "(null)");
    } else {
        bsTestPass();
    }
}


/* Assert a value's rendering "text" (owned) against "expected"; the value and the text are released */
static void bsTestAssertText(const char *file, int line, const char *expr, BSValue value, BSValue text,
                             const char *expected)
{
    bsRelease(value);
    bsTestAssertEqual(file, line, expr, bsStringData(text), expected);
    bsRelease(text);
}


void bsTestAssertValue(const char *file, int line, const char *expr, BSValue value, const char *expectedJSON)
{
    bsTestAssertText(file, line, expr, value, bsJSONEncode(value, 0), expectedJSON);
}


void bsTestAssertValueString(const char *file, int line, const char *expr, BSValue value, const char *expected)
{
    bsTestAssertText(file, line, expr, value, bsValueString(value), expected);
}


/*
 * Test helpers
 */


static BSStringBuilder bsTestLog;
static BSValue bsTestError = {BS_NULL, {0}};


void bsTestLogFn(const char *text, void *data)
{
    bsSBAppendString(&bsTestLog, text);
    bsSBAppendChar(&bsTestLog, '\n');
}


const char *bsTestLogText(void)
{
    return bsTestLog.data != NULL ? bsTestLog.data : "";
}


void bsTestLogClear(void)
{
    bsSBFree(&bsTestLog);
}


const char *bsTestErrorText(void)
{
    return bsTestError.type == BS_STRING ? bsStringData(bsTestError) : NULL;
}


BSOptions *bsTestOptions(void)
{
    BSOptions *options = bsOptionsNew();
    options->logFn = bsTestLogFn;
    return options;
}


BSValue bsTestExecuteOptions(const char *text, BSOptions *options)
{
    bsTestLogClear();
    bsAssign(&bsTestError, bsNull());
    BSParserError parserError = {0};
    BSScript *script = bsParseScript(text, strlen(text), 1, "test.bare", &parserError);
    if (script == NULL) {
        bsAssign(&bsTestError, bsRetain(parserError.message));
        bsParserErrorFree(&parserError);
        return bsNull();
    }
    BSValue result = bsExecuteScript(script, options);
    const char *error = bsErrorGet(options);
    if (error != NULL) {
        bsAssign(&bsTestError, bsStringNew(error));
    }
    bsScriptRelease(script);
    return result;
}


BSValue bsTestExecute(const char *text)
{
    BSOptions *options = bsTestOptions();
    BSValue result = bsTestExecuteOptions(text, options);
    bsOptionsFree(options);
    return result;
}


BSValue bsTestExecuteScript(BSScript *script)
{
    BSOptions *options = bsTestOptions();
    BSValue result = bsExecuteScript(script, options);
    bsOptionsFree(options);
    return result;
}


BSScript *bsTestScriptFromJSON(const char *json, const char *scriptName)
{
    BSValue model = bsJSONDecode(json, strlen(json), NULL);
    BSScript *script = bsScriptFromModel(model, scriptName);
    bsRelease(model);
    return script;
}


/*
 * Temporary files
 */


static char bsTestTempPath[256];


const char *bsTestTempDir(void)
{
    if (bsTestTempPath[0] == '\0') {
        snprintf(bsTestTempPath, sizeof(bsTestTempPath), "build/test-temp-%ld", (long) getpid());
        mkdir("build", 0755);
        mkdir(bsTestTempPath, 0755);
    }
    return bsTestTempPath;
}


const char *bsTestTempFile(const char *name, const char *text)
{
    static char path[512];
    snprintf(path, sizeof(path), "%s/%s", bsTestTempDir(), name);
    FILE *file = fopen(path, "wb");
    if (file != NULL) {
        fwrite(text, 1, strlen(text), file);
        fclose(file);
    }
    return path;
}


static void bsTestTempClear(void)
{
    if (bsTestTempPath[0] == '\0') {
        return;
    }
    char command[512];
    snprintf(command, sizeof(command), "rm -rf '%s'", bsTestTempPath);
    if (system(command) != 0) {
        printf("  WARNING: failed to remove %s\n", bsTestTempPath);
    }
    bsTestTempPath[0] = '\0';
}


/*
 * The test runner
 */


/* Seconds on a monotonic clock */
static double bsTestNow(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}


int bsTestRun(const char *filter, bool quiet)
{
    size_t testCount = 0;
    size_t failCount = 0;
    double timeBegin = bsTestNow();

    bsTestQuiet = quiet;
    for (BSTestCase *testCase = bsTestHead; testCase != NULL; testCase = testCase->next) {
        if (filter != NULL && strstr(testCase->name, filter) == NULL) {
            continue;
        }
        testCount++;
        bsTestCurrent = testCase;
        bsTestFailed = false;

        /* Name the test before it runs, so a crash still says which one */
        if (!quiet) {
            printf("%s ... ", testCase->name);
        }
        fflush(stdout);

        double testBegin = bsTestNow();
        if (setjmp(bsTestJump) == 0) {
            testCase->fn();
        }
        if (bsTestFailed) {
            failCount++;
        } else if (quiet) {
            putchar('.');
        } else {
            printf("ok (%.1fms)\n", (bsTestNow() - testBegin) * 1000);
        }
    }
    bsTestCurrent = NULL;

    bsTestTempClear();
    bsTestLogClear();
    bsAssign(&bsTestError, bsNull());
    bsParserCleanup();
    bsIncludeCleanup();
    bsLibraryCleanup();

    /* The report - a quiet run's failure details come first */
    if (quiet) {
        printf("\n");
        if (bsTestFailText.data != NULL) {
            printf("%s", bsTestFailText.data);
        }
    }
    bsSBFree(&bsTestFailText);
    printf("----------------------------------------------------------------------\n");
    printf("Ran %zu test%s, %zu assertion%s in %.3fs\n\n", testCount, testCount == 1 ? "" : "s",
           bsTestAssertions, bsTestAssertions == 1 ? "" : "s", bsTestNow() - timeBegin);
    if (testCount == 0) {
        printf("NO TESTS RAN\n");
        return 1;
    }
    if (failCount != 0) {
        printf("FAILED (failures=%zu)\n", failCount);
        return 1;
    }
    printf("OK\n");
    return 0;
}


void bsTestObjectFill(BSValue object, const char *format, int begin, int end)
{
    char key[16];
    for (int ix = begin; ix < end; ix++) {
        snprintf(key, sizeof(key), format, ix);
        bsObjectSet(object, key, bsNumber(ix));
    }
}


BSValue bsTestRepeat(const char *prefix, const char *text, size_t count, const char *suffix)
{
    BSStringBuilder sb;
    bsSBInit(&sb);
    if (prefix != NULL) {
        bsSBAppendString(&sb, prefix);
    }
    for (size_t ix = 0; ix < count; ix++) {
        bsSBAppendString(&sb, text);
    }
    if (suffix != NULL) {
        bsSBAppendString(&sb, suffix);
    }
    return bsSBToValue(&sb);
}
