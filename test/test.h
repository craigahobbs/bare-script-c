/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * A minimal unit test framework
 *
 * Tests self-register at load time, so adding a test is a single TEST(name) { ... } block. A
 * failed assertion reports its location, aborts the current test, and continues with the next.
 */

#ifndef BARESCRIPT_TEST_H
#define BARESCRIPT_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "barescript/barescript.h"


typedef struct BSTestCase {
    const char *name;
    void (*fn)(void);
    struct BSTestCase *next;
} BSTestCase;


/* Register a test case - called automatically by the TEST macro */
void bsTestRegister(BSTestCase *testCase);

/*
 * Run the registered tests, optionally filtered by a name substring. By default each test prints a
 * line with its result and duration; quiet prints a dot per test and defers failure details to the
 * end. Returns 0 when every test passes, 1 otherwise, or 1 when the filter matches no test.
 */
int bsTestRun(const char *filter, bool quiet);

/* Report an assertion failure and abort the current test */
void bsTestFail(const char *file, int line, const char *format, ...);

/* Report a passing assertion */
void bsTestPass(void);


/* Define and register a test case */
#define TEST(name) \
    static void bsTestFn_##name(void); \
    static BSTestCase bsTestCase_##name = {#name, bsTestFn_##name, NULL}; \
    __attribute__((constructor)) static void bsTestInit_##name(void) { bsTestRegister(&bsTestCase_##name); } \
    static void bsTestFn_##name(void)


/*
 * Assertions
 */

#define ASSERT_TRUE(expr) bsTestCheck(__FILE__, __LINE__, (expr), "ASSERT_TRUE(" #expr ") failed")
#define ASSERT_FALSE(expr) bsTestCheck(__FILE__, __LINE__, !(expr), "ASSERT_FALSE(" #expr ") failed")
#define ASSERT_NULL(expr) bsTestCheck(__FILE__, __LINE__, (expr) == NULL, "ASSERT_NULL(" #expr ") failed")
#define ASSERT_NOT_NULL(expr) bsTestCheck(__FILE__, __LINE__, (expr) != NULL, "ASSERT_NOT_NULL(" #expr ") failed")

#define ASSERT_INT_EQ(actual, expected) \
    do { \
        long long bsActual_ = (long long) (actual); \
        long long bsExpected_ = (long long) (expected); \
        if (bsActual_ != bsExpected_) { \
            bsTestFail(__FILE__, __LINE__, "%s == %s - actual %lld, expected %lld", \
                       #actual, #expected, bsActual_, bsExpected_); \
        } else { bsTestPass(); } \
    } while (0)

#define ASSERT_DOUBLE_EQ(actual, expected) \
    do { \
        double bsActual_ = (double) (actual); \
        double bsExpected_ = (double) (expected); \
        if (bsActual_ != bsExpected_) { \
            bsTestFail(__FILE__, __LINE__, "%s == %s - actual %.17g, expected %.17g", \
                       #actual, #expected, bsActual_, bsExpected_); \
        } else { bsTestPass(); } \
    } while (0)

#define ASSERT_STR_CONTAINS(haystack, needle) \
    bsTestAssertContains(__FILE__, __LINE__, #haystack " contains " #needle, (haystack), (needle), true)
#define ASSERT_STR_NOT_CONTAINS(haystack, needle) \
    bsTestAssertContains(__FILE__, __LINE__, #haystack " omits " #needle, (haystack), (needle), false)
#define ASSERT_STR_EQ(actual, expected) \
    bsTestAssertEqual(__FILE__, __LINE__, #actual " == " #expected, (actual), (expected))

/* Assert a value's JSON representation - the value is released */
#define ASSERT_VALUE(value, expectedJSON) bsTestAssertValue(__FILE__, __LINE__, #value, (value), (expectedJSON))

/* Assert a value's JSON representation - the value is not released */
#define ASSERT_VALUE_KEEP(value, expectedJSON) \
    bsTestAssertValue(__FILE__, __LINE__, #value, bsRetain(value), (expectedJSON))

/* Assert a value's string representation - the value is released */
#define ASSERT_VALUE_STRING(value, expectedString) \
    bsTestAssertValueString(__FILE__, __LINE__, #value, (value), (expectedString))

/* Assert a value's string representation - the value is not released */
#define ASSERT_VALUE_STRING_KEEP(value, expectedString) \
    bsTestAssertValueString(__FILE__, __LINE__, #value, bsRetain(value), (expectedString))


void bsTestCheck(const char *file, int line, bool ok, const char *message);
void bsTestAssertContains(const char *file, int line, const char *subject, const char *haystack, const char *needle,
                          bool expected);
void bsTestAssertValue(const char *file, int line, const char *expr, BSValue value, const char *expectedJSON);
void bsTestAssertValueString(const char *file, int line, const char *expr, BSValue value, const char *expected);

/* Assert "actual" equals "expected", naming "subject" - the input or expression under test - on failure */
void bsTestAssertEqual(const char *file, int line, const char *subject, const char *actual, const char *expected);


/*
 * Test helpers
 */

/* Execute script text and return its result. Log output is captured - see bsTestLogText. */
BSValue bsTestExecute(const char *text);

/* Execute script text with options and return its result. The captured log text is cleared first. */
BSValue bsTestExecuteOptions(const char *text, BSOptions *options);

/* Create test options with a capturing log function */
BSOptions *bsTestOptions(void);

/* Execute a compiled script with capturing test options and return its owned result */
BSValue bsTestExecuteScript(BSScript *script);

/* Parse script text from line 1 as "scriptName", with no error out; NULL if it does not parse */
BSScript *bsTestScript(const char *text, const char *scriptName);

/* Decode a JSON model and compile it to a script named "scriptName" (NULL keeps the model's); NULL if invalid */
BSScript *bsTestScriptFromJSON(const char *json, const char *scriptName);

/* Get the captured log text of the most recent bsTestExecute or bsTestOptions log function */
const char *bsTestLogText(void);

/* Clear the captured log text */
void bsTestLogClear(void);

/* The capturing log function */
void bsTestLogFn(const char *text, void *data);

/* Get the most recent bsTestExecute runtime error message, or NULL */
const char *bsTestErrorText(void);

/* Create a temporary directory for file system tests; returns a static path */
const char *bsTestTempDir(void);

/* Write a temporary file; returns a static path valid until the next call */
const char *bsTestTempFile(const char *name, const char *text);

/* Set the keys "format" makes of each index in [begin, end) to their index */
void bsTestObjectFill(BSValue object, const char *format, int begin, int end);

/* A string of "prefix", "text" repeated "count" times, and "suffix" - either end may be NULL */
BSValue bsTestRepeat(const char *prefix, const char *text, size_t count, const char *suffix);


#endif
