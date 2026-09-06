/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * Thread tests - every thread is an isolated runtime
 */

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"


#define BS_THREAD_COUNT 8
#define BS_THREAD_ROUNDS 4

typedef struct {
    int index;
    char error[512]; /* the first failure, or empty */
} BSThreadTask;


/* Record a thread's first failure - the test asserts on the main thread, after the join */
static void bsThreadFail(BSThreadTask *task, const char *what, const char *detail)
{
    if (task->error[0] == '\0') {
        snprintf(task->error, sizeof(task->error), "%s: %s", what, detail != NULL ? detail : "(null)");
    }
}


/*
 * Parse, lint, and execute a script that touches every piece of per-thread state - the parser and
 * linter bootstraps, a bundled include, the thread's own system include registry, the intern table
 * and an indexed object (past sixteen keys), the regex match keys, mathRandom - and check the result
 */
static void *bsThreadRun(void *data)
{
    BSThreadTask *task = data;
    char text[1024];
    char expected[128];

    /* This thread's system include - the registry is per thread, so the main thread never sees it */
    snprintf(text, sizeof(text), "threadValue = %d\n", task->index);
    bsSystemIncludeRegister("threadTest.bare", text);

    snprintf(text, sizeof(text),
             "include <url.bare>\n"
             "include <threadTest.bare>\n"
             "\n"
             "function keys(count):\n"
             "    obj = {}\n"
             "    ix = 0\n"
             "    while ix < count:\n"
             "        objectSet(obj, 'key' + ix, ix)\n"
             "        ix = ix + 1\n"
             "    endwhile\n"
             "    return obj\n"
             "endfunction\n"
             "\n"
             "obj = keys(100 + threadValue)\n"
             "match = regexMatch(regexNew('^(?<name>[a-z]+)-([0-9]+)$'), 'thread-' + threadValue)\n"
             "random = mathRandom()\n"
             "if random < 0 || random >= 1:\n"
             "    return 'bad random ' + random\n"
             "endif\n"
             "groups = objectGet(match, 'groups')\n"
             "return urlEncodeComponent('a b') + '|' + threadValue + '|' + objectGet(groups, 'name') + '|' + \\\n"
             "    objectGet(groups, '2') + '|' + arrayLength(objectKeys(obj)) + '|' + \\\n"
             "    jsonStringify({'b': [1, {'c': null}], 'a': objectGet(obj, 'key' + threadValue)})\n");
    snprintf(expected, sizeof(expected), "a%%20b|%d|thread|%d|%d|{\"a\":%d,\"b\":[1,{\"c\":null}]}",
             task->index, task->index, 100 + task->index, task->index);

    for (int round = 0; round < BS_THREAD_ROUNDS && task->error[0] == '\0'; round++) {
        BSParserError error = {0};
        BSScript *script = bsParseScript(text, strlen(text), 1, "thread.bare", &error);
        if (script == NULL) {
            bsThreadFail(task, "parse", bsStringData(error.message));
            bsParserErrorFree(&error);
            break;
        }

        BSValue warnings = bsLintScript(script, bsNull());
        if (warnings.type != BS_ARRAY) {
            bsThreadFail(task, "lint", "no warnings array");
        }
        bsRelease(warnings);

        BSOptions *options = bsOptionsNew();
        BSValue result = bsExecuteScript(script, options);
        if (bsErrorGet(options) != NULL) {
            bsThreadFail(task, "execute", bsErrorGet(options));
        } else if (result.type != BS_STRING) {
            bsThreadFail(task, "result", "not a string");
        } else if (strcmp(bsStringData(result), expected) != 0) {
            bsThreadFail(task, "result", bsStringData(result));
        }
        bsRelease(result);
        bsOptionsFree(options);
        bsScriptRelease(script);
    }

    /* An expression, and a parse error */
    BSParserError error = {0};
    BSExpr *expr = bsParseExpression("1 + 2 * 3", 9, 1, NULL, false, &error);
    if (expr == NULL) {
        bsThreadFail(task, "parse expression", bsStringData(error.message));
        bsParserErrorFree(&error);
    } else {
        BSOptions *options = bsOptionsNew();
        BSValue result = bsEvaluateExpression(expr, options, NULL, true);
        if (result.type != BS_NUMBER || result.u.number != 7) {
            bsThreadFail(task, "evaluate expression", "not 7");
        }
        bsRelease(result);
        bsOptionsFree(options);
        bsExprFree(expr);
    }
    BSScript *bad = bsParseScript("foo bar", 7, 1, "bad.bare", &error);
    if (bad != NULL) {
        bsThreadFail(task, "parse error", "no error");
        bsScriptRelease(bad);
    } else {
        if (error.message.type != BS_STRING || strstr(bsStringData(error.message), "Syntax error") == NULL) {
            bsThreadFail(task, "parse error", "no syntax error message");
        }
        bsParserErrorFree(&error);
    }

    /* This thread's connection pool - each fetch of a refused port runs a transfer through it */
    for (int ix = 0; ix < 25; ix++) {
        BSFetchRequest request = {.url = "http://127.0.0.1:1/nope", .headers = bsNull()};
        BSValue response = bsNull();
        bsFetchHTTP(&request, &response, 1, NULL);
        if (response.type != BS_NULL) {
            bsThreadFail(task, "fetch", "a refused connection returned a response");
            bsRelease(response);
        }
    }

    /* Release this thread's runtime state */
    bsParserCleanup();
    bsSystemIncludeClear();
    bsIncludeCleanup();
    bsLibraryCleanup();
    bsValueCleanup();
    return NULL;
}


TEST(thread_isolated_runtimes)
{
    /* The main thread's runtime state, before... */
    ASSERT_VALUE(bsTestExecute("include <url.bare>\nreturn urlEncodeComponent('a b')"), "\"a%20b\"");

    /* The workers' fetches must leave the process's SIGPIPE disposition as they found it */
    struct sigaction before;
    sigaction(SIGPIPE, NULL, &before);

    pthread_t threads[BS_THREAD_COUNT];
    BSThreadTask tasks[BS_THREAD_COUNT];
    for (int ix = 0; ix < BS_THREAD_COUNT; ix++) {
        memset(&tasks[ix], 0, sizeof(tasks[ix]));
        tasks[ix].index = ix;
        ASSERT_INT_EQ(pthread_create(&threads[ix], NULL, bsThreadRun, &tasks[ix]), 0);
    }
    for (int ix = 0; ix < BS_THREAD_COUNT; ix++) {
        ASSERT_INT_EQ(pthread_join(threads[ix], NULL), 0);
    }
    for (int ix = 0; ix < BS_THREAD_COUNT; ix++) {
        if (tasks[ix].error[0] != '\0') {
            bsTestFail(__FILE__, __LINE__, "thread %d - %s", ix, tasks[ix].error);
        }
    }

    struct sigaction after;
    sigaction(SIGPIPE, NULL, &after);
    ASSERT_TRUE(after.sa_handler == before.sa_handler);

    /* ...and after: intact, and without the workers' system include */
    ASSERT_VALUE(bsTestExecute("include <url.bare>\nreturn urlEncodeComponent('a b')"), "\"a%20b\"");
    ASSERT_VALUE(bsTestExecute("include <threadTest.bare>\nreturn threadValue"), "null");
    ASSERT_STR_CONTAINS(bsTestErrorText(), "threadTest.bare");
}
