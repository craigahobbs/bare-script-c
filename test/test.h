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

#include "barescript/barescript.h"
#include "barescript/includeSource.h"


typedef struct BSTestCase {
    const char *name;
    void (*fn)(void);
    struct BSTestCase *next;
} BSTestCase;


/* Register a test case - called automatically by the TEST macro */
void bsTestRegister(BSTestCase *testCase);

/* Run the registered tests, optionally filtered by a name substring */
int bsTestRun(const char *filter);

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

#define ASSERT_TRUE(expr) \
    do { \
        if (!(expr)) { bsTestFail(__FILE__, __LINE__, "ASSERT_TRUE(%s) failed", #expr); } \
        else { bsTestPass(); } \
    } while (0)

#define ASSERT_FALSE(expr) \
    do { \
        if (expr) { bsTestFail(__FILE__, __LINE__, "ASSERT_FALSE(%s) failed", #expr); } \
        else { bsTestPass(); } \
    } while (0)

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
        if (!bsTestDoubleEqual(bsActual_, bsExpected_)) { \
            bsTestFail(__FILE__, __LINE__, "%s == %s - actual %.17g, expected %.17g", \
                       #actual, #expected, bsActual_, bsExpected_); \
        } else { bsTestPass(); } \
    } while (0)

#define ASSERT_STR_EQ(actual, expected) \
    do { \
        const char *bsActual_ = (actual); \
        const char *bsExpected_ = (expected); \
        if (!bsTestStringEqual(bsActual_, bsExpected_)) { \
            bsTestFail(__FILE__, __LINE__, "%s == %s\n    actual:   %s\n    expected: %s", \
                       #actual, #expected, bsActual_ ? bsActual_ : "(null)", bsExpected_ ? bsExpected_ : "(null)"); \
        } else { bsTestPass(); } \
    } while (0)

#define ASSERT_NULL(expr) \
    do { \
        if ((expr) != NULL) { bsTestFail(__FILE__, __LINE__, "ASSERT_NULL(%s) failed", #expr); } \
        else { bsTestPass(); } \
    } while (0)

#define ASSERT_NOT_NULL(expr) \
    do { \
        if ((expr) == NULL) { bsTestFail(__FILE__, __LINE__, "ASSERT_NOT_NULL(%s) failed", #expr); } \
        else { bsTestPass(); } \
    } while (0)

/* Assert a value's JSON representation - the value is released */
#define ASSERT_VALUE(value, expectedJSON) bsTestAssertValue(__FILE__, __LINE__, #value, (value), (expectedJSON))

/* Assert a value's JSON representation - the value is not released */
#define ASSERT_VALUE_KEEP(value, expectedJSON) \
    bsTestAssertValue(__FILE__, __LINE__, #value, bsRetain(value), (expectedJSON))

/* Assert a value's string representation - the value is released */
#define ASSERT_VALUE_STRING(value, expectedString) \
    bsTestAssertValueString(__FILE__, __LINE__, #value, (value), (expectedString))


bool bsTestDoubleEqual(double actual, double expected);
bool bsTestStringEqual(const char *actual, const char *expected);
void bsTestAssertValue(const char *file, int line, const char *expr, BSValue value, const char *expectedJSON);
void bsTestAssertValueString(const char *file, int line, const char *expr, BSValue value, const char *expected);


/*
 * Test helpers
 */

/* Execute script text and return its result. Log output is captured - see bsTestLogText. */
BSValue bsTestExecute(const char *text);

/* Execute script text with options and return its result */
BSValue bsTestExecuteOptions(const char *text, BSOptions *options);

/* Create test options with a capturing log function */
BSOptions *bsTestOptions(void);

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

/* Remove the temporary directory and its contents */
void bsTestTempClear(void);


#endif
