/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The unit test framework
 */

#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test.h"


/* The registered test cases, in registration order */
static BSTestCase *bsTestHead = NULL;
static BSTestCase *bsTestTail = NULL;

/* The current test's state */
static jmp_buf bsTestJump;
static bool bsTestRunning = false;
static bool bsTestFailed = false;
static size_t bsTestAssertions = 0;
static size_t bsTestFailures = 0;


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
    bsTestFailures++;
    printf("  FAIL %s:%d: ", file, line);
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    if (bsTestRunning) {
        longjmp(bsTestJump, 1);
    }
}


bool bsTestDoubleEqual(double actual, double expected)
{
    if (isnan(actual) && isnan(expected)) {
        return true;
    }
    return actual == expected;
}


bool bsTestStringEqual(const char *actual, const char *expected)
{
    if (actual == NULL || expected == NULL) {
        return actual == expected;
    }
    return strcmp(actual, expected) == 0;
}


/* Assert a value's rendering "text" (owned) against "expected"; the value and the text are released */
static void bsTestAssertText(const char *file, int line, const char *expr, BSValue value, BSValue text,
                             const char *expected)
{
    bsRelease(value);
    if (!bsTestStringEqual(bsStringData(text), expected)) {
        bsTestFail(file, line, "%s\n    actual:   %s\n    expected: %s", expr, bsStringData(text), expected);
    } else {
        bsTestPass();
    }
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
    bsAssign(&bsTestError, bsNull());
    BSParserError parserError;
    memset(&parserError, 0, sizeof(parserError));
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
    bsTestLogClear();
    BSOptions *options = bsTestOptions();
    BSValue result = bsTestExecuteOptions(text, options);
    bsOptionsFree(options);
    return result;
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


int bsTestRun(const char *filter)
{
    size_t testCount = 0;
    size_t failCount = 0;
    BSTestCase *testCase;

    for (testCase = bsTestHead; testCase != NULL; testCase = testCase->next) {
        if (filter != NULL && strstr(testCase->name, filter) == NULL) {
            continue;
        }
        testCount++;
        bsTestFailed = false;
        bsTestRunning = true;
        if (setjmp(bsTestJump) == 0) {
            testCase->fn();
        }
        bsTestRunning = false;
        if (bsTestFailed) {
            failCount++;
            printf("  in test \"%s\"\n", testCase->name);
        }
    }

    bsTestTempClear();
    bsTestLogClear();
    bsAssign(&bsTestError, bsNull());
    bsParserCleanup();
    bsSystemIncludeClear();
    bsIncludeCleanup();
    bsLibraryCleanup();

    printf("\n%zu test%s, %zu assertion%s, %zu failure%s\n", testCount, testCount == 1 ? "" : "s",
           bsTestAssertions, bsTestAssertions == 1 ? "" : "s", bsTestFailures,
           bsTestFailures == 1 ? "" : "s");
    return failCount != 0 ? 1 : 0;
}


char *bsTestStrdup(const char *text)
{
    size_t size = strlen(text) + 1;
    char *result = malloc(size);
    memcpy(result, text, size);
    return result;
}


void bsTestObjectFill(BSValue object, const char *format, int begin, int end)
{
    char key[16];
    for (int ix = begin; ix < end; ix++) {
        snprintf(key, sizeof(key), format, ix);
        bsObjectSet(object, key, bsNumber(ix));
    }
}
