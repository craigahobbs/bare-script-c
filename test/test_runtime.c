/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The runtime unit tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"


TEST(runtime_version)
{
    ASSERT_STR_EQ(bsVersion(), BARESCRIPT_VERSION);
}


TEST(runtime_expressions)
{
    ASSERT_VALUE(bsTestExecute("return 1 + 2"), "3");
    ASSERT_VALUE(bsTestExecute("return 5 - 3"), "2");
    ASSERT_VALUE(bsTestExecute("return 2 * 3"), "6");
    ASSERT_VALUE(bsTestExecute("return 7 / 2"), "3.5");
    ASSERT_VALUE(bsTestExecute("return 7 % 2"), "1");
    ASSERT_VALUE(bsTestExecute("return -7 % 3"), "-1");
    ASSERT_VALUE(bsTestExecute("return -4 % 2"), "0");
    ASSERT_VALUE(bsTestExecute("return 5.5 % 2"), "1.5");
    ASSERT_VALUE(bsTestExecute("return -5.5 % 2"), "-1.5");
    ASSERT_VALUE(bsTestExecute("return 7 % 2.5"), "2");
    ASSERT_VALUE(bsTestExecute("return 2 ** 3"), "8");
    ASSERT_VALUE(bsTestExecute("return 2 ** 3 ** 2"), "64");
    ASSERT_VALUE(bsTestExecute("return -2 ** 2"), "4");
    ASSERT_VALUE(bsTestExecute("return 1 / 0"), "null");
    ASSERT_VALUE(bsTestExecute("return 0 / 0"), "null");
    ASSERT_VALUE(bsTestExecute("return 1 % 0"), "null");
    ASSERT_VALUE(bsTestExecute("return (-8) ** 0.5"), "null");
    ASSERT_VALUE(bsTestExecute("return 1e308 * 10"), "null");
    ASSERT_VALUE(bsTestExecute("return 1e308 + 1e308"), "null");
    ASSERT_VALUE(bsTestExecute("return 1e308 - -1e308"), "null");
}


TEST(runtime_expression_strings)
{
    ASSERT_VALUE(bsTestExecute("return 'a' + 'b'"), "\"ab\"");
    ASSERT_VALUE(bsTestExecute("return 'a' + 1"), "\"a1\"");
    ASSERT_VALUE(bsTestExecute("return 1 + 'a'"), "\"1a\"");
    ASSERT_VALUE(bsTestExecute("return 'a' + null"), "\"anull\"");
    ASSERT_VALUE(bsTestExecute("return true + 'x'"), "\"truex\"");
    ASSERT_VALUE(bsTestExecute("return 'a' - 'b'"), "null");
    ASSERT_VALUE(bsTestExecute("return 'a' * 2"), "null");
    ASSERT_VALUE(bsTestExecute("return 'a' / 2"), "null");
    ASSERT_VALUE(bsTestExecute("return 'a' % 2"), "null");
    ASSERT_VALUE(bsTestExecute("return 'a' ** 2"), "null");
}


TEST(runtime_expression_datetime)
{
    ASSERT_VALUE(bsTestExecute("return datetimeNew(2026, 1, 2) - datetimeNew(2026, 1, 1)"), "86400000");
    ASSERT_VALUE(bsTestExecute("return datetimeYear(datetimeNew(2026, 1, 1) + 86400000)"), "2026");
    ASSERT_VALUE(bsTestExecute("return datetimeDay(86400000 + datetimeNew(2026, 1, 1))"), "2");
    ASSERT_VALUE(bsTestExecute("return datetimeNew(2026, 1, 1) + 1e300"), "null");
    ASSERT_VALUE(bsTestExecute("return 1e300 + datetimeNew(2026, 1, 1)"), "null");
    ASSERT_VALUE(bsTestExecute("return true + 1"), "null");
    ASSERT_VALUE(bsTestExecute("return datetimeNew(2026, 1, 1) - 1"), "null");
}


TEST(runtime_expression_bitwise)
{
    ASSERT_VALUE(bsTestExecute("return 5 & 3"), "1");
    ASSERT_VALUE(bsTestExecute("return 5 | 3"), "7");
    ASSERT_VALUE(bsTestExecute("return 5 ^ 3"), "6");
    ASSERT_VALUE(bsTestExecute("return 1 << 4"), "16");
    ASSERT_VALUE(bsTestExecute("return -16 >> 2"), "-4");
    ASSERT_VALUE(bsTestExecute("return ~5"), "-6");
    ASSERT_VALUE(bsTestExecute("return 4294967296 & 1"), "0");
    ASSERT_VALUE(bsTestExecute("return -1 & 4294967295"), "-1");
    ASSERT_VALUE(bsTestExecute("return 1 << 33"), "2");

    /* Non-integer operands are invalid */
    ASSERT_VALUE(bsTestExecute("return 1.5 & 1"), "null");
    ASSERT_VALUE(bsTestExecute("return 1 & 1.5"), "null");
    ASSERT_VALUE(bsTestExecute("return 'a' & 1"), "null");
    ASSERT_VALUE(bsTestExecute("return ~1.5"), "null");
    ASSERT_VALUE(bsTestExecute("return -'a'"), "null");
}


TEST(runtime_expression_comparison)
{
    ASSERT_VALUE(bsTestExecute("return 1 < 2"), "true");
    ASSERT_VALUE(bsTestExecute("return 'a' < 'b'"), "true");
    ASSERT_VALUE(bsTestExecute("return 1 <= 1"), "true");
    ASSERT_VALUE(bsTestExecute("return 'a' <= 'a'"), "true");
    ASSERT_VALUE(bsTestExecute("return 2 > 1"), "true");
    ASSERT_VALUE(bsTestExecute("return 'b' > 'a'"), "true");
    ASSERT_VALUE(bsTestExecute("return 1 >= 1"), "true");
    ASSERT_VALUE(bsTestExecute("return 'a' >= 'a'"), "true");
    ASSERT_VALUE(bsTestExecute("return 1 == 1"), "true");
    ASSERT_VALUE(bsTestExecute("return 'a' == 'a'"), "true");
    ASSERT_VALUE(bsTestExecute("return 1 != 2"), "true");
    ASSERT_VALUE(bsTestExecute("return 'a' != 'b'"), "true");
    ASSERT_VALUE(bsTestExecute("return [1, [2, 3]] == [1, [2, 3]]"), "true");
    ASSERT_VALUE(bsTestExecute("return {'b': 2, 'a': 1} == {'a': 1, 'b': 2}"), "true");
    ASSERT_VALUE(bsTestExecute("return null < 1"), "true");
}


TEST(runtime_expression_logical)
{
    ASSERT_VALUE(bsTestExecute("return 0 && 1"), "0");
    ASSERT_VALUE(bsTestExecute("return 1 && 2"), "2");
    ASSERT_VALUE(bsTestExecute("return 0 || 3"), "3");
    ASSERT_VALUE(bsTestExecute("return 4 || 5"), "4");
    ASSERT_VALUE(bsTestExecute("return !0"), "true");
    ASSERT_VALUE(bsTestExecute("return !1"), "false");
    ASSERT_VALUE(bsTestExecute("return -(1)"), "-1");

    /* Short circuiting does not evaluate the right operand */
    ASSERT_VALUE(bsTestExecute("return false && undefinedFunc()"), "false");
    ASSERT_VALUE(bsTestExecute("return true || undefinedFunc()"), "true");
}


TEST(runtime_special_variables)
{
    ASSERT_VALUE(bsTestExecute("return null"), "null");
    ASSERT_VALUE(bsTestExecute("return true"), "true");
    ASSERT_VALUE(bsTestExecute("return false"), "false");
    ASSERT_VALUE(bsTestExecute("null = 1\nreturn null"), "null");
    ASSERT_VALUE(bsTestExecute("return undefinedVariable"), "null");
}


TEST(runtime_statements)
{
    ASSERT_VALUE(bsTestExecute("a = 1\nb = 2\nreturn a + b"), "3");
    ASSERT_VALUE(bsTestExecute("systemLog('x')"), "null");
    ASSERT_STR_EQ(bsTestLogText(), "x\n");
    ASSERT_VALUE(bsTestExecute("return"), "null");
    ASSERT_VALUE(bsTestExecute(""), "null");
    ASSERT_VALUE(bsTestExecute("label:\nreturn 1"), "1");
}


TEST(runtime_control_flow)
{
    ASSERT_VALUE(bsTestExecute("a = 1\nif a > 0:\n    return 'pos'\nelif a < 0:\n    return 'neg'\n"
                               "else:\n    return 'zero'\nendif"), "\"pos\"");
    ASSERT_VALUE(bsTestExecute("a = -1\nif a > 0:\n    return 'pos'\nelif a < 0:\n    return 'neg'\n"
                               "else:\n    return 'zero'\nendif"), "\"neg\"");
    ASSERT_VALUE(bsTestExecute("a = 0\nif a > 0:\n    return 'pos'\nelif a < 0:\n    return 'neg'\n"
                               "else:\n    return 'zero'\nendif"), "\"zero\"");
    ASSERT_VALUE(bsTestExecute("if false:\n    return 1\nendif\nreturn 2"), "2");
    ASSERT_VALUE(bsTestExecute("i = 0\nsum = 0\nwhile i < 10:\n    sum = sum + i\n    i = i + 1\n"
                               "endwhile\nreturn sum"), "45");
    ASSERT_VALUE(bsTestExecute("while false:\n    return 1\nendwhile\nreturn 2"), "2");
    ASSERT_VALUE(bsTestExecute("sum = 0\nfor v in [1, 2, 3]:\n    sum = sum + v\nendfor\nreturn sum"), "6");
    ASSERT_VALUE(bsTestExecute("sum = 0\nfor v, i in [1, 2, 3]:\n    sum = sum + i * v\nendfor\nreturn sum"), "8");
    ASSERT_VALUE(bsTestExecute("for v in []:\n    return 1\nendfor\nreturn 2"), "2");
    ASSERT_VALUE(bsTestExecute("for v in 'nope':\n    return 1\nendfor\nreturn 2"), "2");
    ASSERT_VALUE(bsTestExecute("i = 0\nwhile true:\n    i = i + 1\n    if i > 5:\n        break\n"
                               "    endif\nendwhile\nreturn i"), "6");
    ASSERT_VALUE(bsTestExecute("sum = 0\nfor v in [1, -2, 3]:\n    if v < 0:\n        continue\n"
                               "    endif\n    sum = sum + v\nendfor\nreturn sum"), "4");
    ASSERT_VALUE(bsTestExecute("sum = 0\ni = 0\nwhile i < 5:\n    i = i + 1\n    if i == 3:\n"
                               "        continue\n    endif\n    sum = sum + i\nendwhile\nreturn sum"), "12");

    /* Jump and label statements */
    ASSERT_VALUE(bsTestExecute("values = [1, 2, 3]\nsum = 0\nix = 0\nloop:\n"
                               "    jumpif (ix >= arrayLength(values)) done\n"
                               "    sum = sum + arrayGet(values, ix)\n    ix = ix + 1\njump loop\ndone:\n"
                               "return sum"), "6");
    ASSERT_VALUE(bsTestExecute("jumpif (false) nowhere\nreturn 1\nnowhere:\nreturn 2"), "1");
}


TEST(runtime_functions)
{
    ASSERT_VALUE(bsTestExecute("function f(a, b):\n    return a + b\nendfunction\nreturn f(1, 2)"), "3");
    ASSERT_VALUE(bsTestExecute("function f(a, b):\n    return a\nendfunction\nreturn f(1)"), "1");
    ASSERT_VALUE(bsTestExecute("function f(a, b):\n    return b\nendfunction\nreturn f(1)"), "null");
    ASSERT_VALUE(bsTestExecute("function f():\nendfunction\nreturn f()"), "null");
    ASSERT_VALUE(bsTestExecute("function f(a, rest...):\n    return rest\nendfunction\nreturn f(1, 2, 3)"),
                 "[2,3]");
    ASSERT_VALUE(bsTestExecute("function f(a, rest...):\n    return rest\nendfunction\nreturn f(1)"), "[]");
    ASSERT_VALUE(bsTestExecute("function f(a, rest...):\n    return a\nendfunction\nreturn f()"), "null");
    ASSERT_VALUE(bsTestExecute("function f(...):\n    return 1\nendfunction\nreturn f()"), "1");

    /* Recursion and function values */
    ASSERT_VALUE(bsTestExecute("function fact(n):\n    return if(n <= 1, 1, n * fact(n - 1))\n"
                               "endfunction\nreturn fact(5)"), "120");
    ASSERT_VALUE(bsTestExecute("function f(a):\n    return a\nendfunction\ng = f\nreturn g(7)"), "7");

    /* Redefining a function must not keep serving the previous definition */
    ASSERT_VALUE(bsTestExecute("function f():\n    return 1\nendfunction\na = f()\n"
                               "function f():\n    return 2\nendfunction\nreturn [a, f()]"), "[1,2]");

    /* Repeated calls of a global function (the call-site cache's hit path) */
    ASSERT_VALUE(bsTestExecute("function f():\n    return 1\nendfunction\n"
                               "i = 0\ns = 0\nwhile i < 3:\n    s = s + f()\n    i = i + 1\n"
                               "endwhile\nreturn s"), "3");

    /* A function value in a local slot is called through the slot, not the globals cache */
    ASSERT_VALUE(bsTestExecute("function inc(n):\n    return n + 1\nendfunction\n"
                               "function apply(fn, x):\n    return fn(x)\nendfunction\n"
                               "return apply(inc, 4)"), "5");

    /* An unassigned local of the same name falls through to the global function */
    ASSERT_VALUE(bsTestExecute("function f(c):\n    if c:\n        mathAbs = 1\n    endif\n"
                               "    return mathAbs(-4)\nendfunction\nreturn f(false)"), "4");

    /* An override of a library function must not keep using the intrinsic */
    ASSERT_VALUE(bsTestExecute("function mathSqrt(x):\n    return x\nendfunction\nreturn mathSqrt(9)"), "9");
    /* Same call site after an in-place override - the warm cache still holds, generation unchanged */
    ASSERT_VALUE(bsTestExecute(
        "i = 0\nr = 0\nwhile i < 2:\n    if i == 1:\n        arrayGet = 1\n    endif\n"
        "    r = arrayGet([9], 0)\n    i = i + 1\nendwhile\nreturn r"), "null");
    ASSERT_VALUE(bsTestExecute(
        "i = 0\nr = 0\nwhile i < 2:\n    if i == 1:\n        function arrayGet(a, i):\n"
        "            return 99\n        endfunction\n    endif\n"
        "    r = arrayGet([9], 0)\n    i = i + 1\nendwhile\nreturn r"), "99");
    ASSERT_VALUE(bsTestExecute("function objectHas(o, k):\n    return 'ov'\nendfunction\n"
                               "return objectHas({}, 'a')"), "\"ov\"");
    ASSERT_VALUE(bsTestExecute("function objectSet(o, k, v):\n    return 'ov'\nendfunction\n"
                               "return objectSet({}, 'a', 1)"), "\"ov\"");
    ASSERT_VALUE(bsTestExecute("function arrayNew():\n    return 'ov'\nendfunction\nreturn arrayNew()"),
                 "\"ov\"");
    ASSERT_VALUE(bsTestExecute("function objectNew():\n    return 'ov'\nendfunction\nreturn objectNew()"),
                 "\"ov\"");
    ASSERT_VALUE(bsTestExecute("function arrayLength(a):\n    return 'ov'\nendfunction\n"
                               "return arrayLength([])"), "\"ov\"");
    ASSERT_VALUE(bsTestExecute("function regexMatch(r, s):\n    return 'ov'\nendfunction\n"
                               "return regexMatch(regexNew('a'), 'a')"), "\"ov\"");

    /* A call's borrowed arguments survive a later argument's side effects */
    ASSERT_VALUE(bsTestExecute("o = {'k': 1}\nfunction mut(obj):\n    objectSet(obj, 'k', 9)\n"
                               "    return 'k'\nendfunction\nreturn objectGet(o, mut(o), 0)"), "9");
    ASSERT_VALUE(bsTestExecute("o = {}\nfunction val():\n    objectSet(o, 'a', 1)\n    return 2\n"
                               "endfunction\nreturn [objectSet(o, 'b', val()), o]"),
                 "[2,{\"a\":1,\"b\":2}]");

    /* Function-local variables shadow globals but assignments stay local */
    ASSERT_VALUE(bsTestExecute("x = 1\nfunction f():\n    x = 2\n    return x\nendfunction\n"
                               "return [f(), x]"), "[2,1]");
    ASSERT_VALUE(bsTestExecute("x = 1\nfunction f():\n    return x\nendfunction\nreturn f()"), "1");
    ASSERT_VALUE(bsTestExecute("x = 1\nfunction f(c):\n    if c:\n        x = 2\n    endif\n"
                               "    return x\nendfunction\nreturn [f(false), f(true)]"), "[1,2]");
    ASSERT_VALUE(bsTestExecute("x = 1\nfunction f():\n    systemGlobalSet('x', 2)\n    return systemGlobalGet('x')\n"
                               "endfunction\nreturn [f(), x]"), "[2,2]");

    /* The math opcode: each function warm, then the misses - a non-number, a negative square root */
    ASSERT_VALUE(bsTestExecute("function f(x):\n    return [mathAbs(x), mathCeil(x), mathFloor(x), mathSign(x), mathSqrt(x)]\n"
                               "endfunction\nf(4)\nreturn [f(-2.5), f(2.25), f(0), f('x'), f(-4)]"),
                 "[[2.5,-2,-3,-1,null],[2.25,3,2,1,1.5],[0,0,0,0,0],[null,null,null,null,null],[4,-4,-4,-1,null]]");
    ASSERT_VALUE(bsTestExecute("function mathSqrt(x):\n    return 'mine'\nendfunction\nreturn [mathSqrt(4), mathAbs(-1)]"),
                 "[\"mine\",1]");
    /* Same site after an in-place override - no global added in between, so the site stays warm */
    ASSERT_VALUE(bsTestExecute("function f(x):\n    return mathAbs(x)\nendfunction\na = 0\na = f(-1)\nmathAbs = 3\n"
                               "return [a, f(-1)]"), "[1,null]");

    /* Function-local jump labels */
    ASSERT_VALUE(bsTestExecute("function f():\n    i = 0\n    loop:\n    i = i + 1\n"
                               "    jumpif (i < 3) loop\n    return i\nendfunction\nreturn f()"), "3");

    /* A function with many locals */
    {
        char text[4096];
        size_t n = 0;
        n += (size_t) snprintf(text + n, sizeof(text) - n, "function f():\n");
        for (int ix = 0; ix < 70; ix++) {
            n += (size_t) snprintf(text + n, sizeof(text) - n, "    v%d = %d\n", ix, ix);
        }
        n += (size_t) snprintf(text + n, sizeof(text) - n, "    return v69\nendfunction\nreturn f()");
        ASSERT_VALUE(bsTestExecute(text), "69");
    }
}


TEST(runtime_builtin_if)
{
    ASSERT_VALUE(bsTestExecute("return if(true, 1, 2)"), "1");
    ASSERT_VALUE(bsTestExecute("return if(1, 2, 3, 4, 5)"), "2");
    ASSERT_VALUE(bsTestExecute("return if(false, 1, 2)"), "2");
    ASSERT_VALUE(bsTestExecute("return if(true, 1)"), "1");
    ASSERT_VALUE(bsTestExecute("return if(false, 1)"), "null");
    ASSERT_VALUE(bsTestExecute("return if(true)"), "null");
    ASSERT_VALUE(bsTestExecute("return if()"), "null");
    ASSERT_VALUE(bsTestExecute("return if(false, undefinedFunc(), 'safe')"), "\"safe\"");
    ASSERT_VALUE(bsTestExecute("return if(true, 'safe', undefinedFunc())"), "\"safe\"");
}


TEST(runtime_errors)
{
    ASSERT_VALUE(bsTestExecute("undefinedFunc()"), "null");
    ASSERT_STR_EQ(bsTestErrorText(), "test.bare:1: Undefined function \"undefinedFunc\"");

    /* A statement without a line number omits the line from the prefix */
    static const char *zeroLine =
        "{\"scriptName\":\"z.bare\",\"statements\":[{\"expr\":{\"expr\":{\"function\":"
        "{\"name\":\"nope\",\"args\":[]}}}}]}";
    BSScript *zeroScript = bsTestScriptFromJSON(zeroLine, "z.bare");
    BSOptions *zeroOptions = bsTestOptions();
    ASSERT_VALUE(bsExecuteScript(zeroScript, zeroOptions), "null");
    ASSERT_STR_EQ(bsErrorGet(zeroOptions), "z.bare: Undefined function \"nope\"");
    bsOptionsFree(zeroOptions);
    bsScriptRelease(zeroScript);
    ASSERT_VALUE(bsTestExecute("jump nowhere"), "null");
    ASSERT_STR_EQ(bsTestErrorText(), "test.bare:1: Unknown jump label \"nowhere\"");
    ASSERT_VALUE(bsTestExecute("a = 1\nundefinedFunc()"), "null");
    ASSERT_STR_EQ(bsTestErrorText(), "test.bare:2: Undefined function \"undefinedFunc\"");

    /* A runtime error inside a function propagates */
    ASSERT_VALUE(bsTestExecute("function f():\n    undefinedFunc()\nendfunction\nf()"), "null");
    ASSERT_STR_EQ(bsTestErrorText(), "test.bare:2: Undefined function \"undefinedFunc\"");

    /* Calling a non-function value is not a runtime error */
    ASSERT_VALUE(bsTestExecute("x = 1\nreturn x()"), "null");
    ASSERT_NULL(bsTestErrorText());

    /* A parse error */
    ASSERT_VALUE(bsTestExecute("a = 1 +"), "null");
    ASSERT_STR_EQ(bsTestErrorText(), "test.bare:1: Syntax error\na = 1 +\n       ^\n");
}


TEST(runtime_max_statements)
{
    BSOptions *options = bsTestOptions();
    options->maxStatements = 10;
    ASSERT_VALUE(bsTestExecuteOptions("i = 0\nwhile i < 100:\n    i = i + 1\nendwhile", options), "null");
    ASSERT_STR_EQ(bsTestErrorText(), "test.bare:4: Exceeded maximum script statements (10)");
    bsOptionsFree(options);

    /* Zero disables the limit */
    options = bsTestOptions();
    options->maxStatements = 0;
    ASSERT_VALUE(bsTestExecuteOptions("i = 0\nwhile i < 100:\n    i = i + 1\nendwhile\nreturn i", options),
                 "100");
    bsOptionsFree(options);
}


TEST(runtime_depth_limit)
{
    /*
     * A deeply nested expression is rejected rather than overflowing the C stack. The parser is
     * itself a BareScript script, so the depth limit stops it while parsing.
     */
    BSValue open = bsTestRepeat("return ", "(", 600, "1");
    BSValue text = bsTestRepeat(bsStringData(open), ")", 600, NULL);
    bsRelease(open);
    ASSERT_VALUE(bsTestExecute(bsStringData(text)), "null");
    ASSERT_STR_EQ(bsTestErrorText(), "test.bare: Maximum expression depth exceeded\n");
    bsRelease(text);
}


TEST(runtime_error_helpers)
{
    BSOptions *options = bsOptionsNew();
    ASSERT_NULL(bsErrorGet(options));
    bsErrorSet(options, "error %d", 1);
    ASSERT_STR_EQ(bsErrorGet(options), "error 1");

    /* The first error wins */
    bsErrorSet(options, "error %d", 2);
    ASSERT_STR_EQ(bsErrorGet(options), "error 1");
    bsErrorSetStatement(options, NULL, 0, "error %d", 3);
    ASSERT_STR_EQ(bsErrorGet(options), "error 1");
    bsErrorClear(options);
    ASSERT_NULL(bsErrorGet(options));

    /* Without a script and statement there is no location prefix */
    bsErrorSetStatement(options, NULL, 0, "plain");
    ASSERT_STR_EQ(bsErrorGet(options), "plain");
    bsErrorClear(options);

    /* Logging is a no-op without a log function */
    bsLog(options, "ignored");
    options->logFn = bsTestLogFn;
    bsTestLogClear();
    bsLog(options, "logged %d", 5);
    ASSERT_STR_EQ(bsTestLogText(), "logged 5\n");
    bsOptionsFree(options);
    bsOptionsFree(NULL);
}


TEST(runtime_evaluate_expression)
{
    BSOptions *options = bsTestOptions();
    BSExpr *expr = bsParseExpression("1 + 2", 5, 0, NULL, false, NULL);
    ASSERT_VALUE(bsEvaluateExpression(expr, options, bsNull(), false), "3");
    bsExprFree(expr);

    /* The built-in expression function aliases */
    expr = bsParseExpression("max(1, 5, 3)", 12, 0, NULL, false, NULL);
    ASSERT_VALUE(bsEvaluateExpression(expr, options, bsNull(), true), "5");
    ASSERT_VALUE(bsEvaluateExpression(expr, options, bsNull(), false), "null");
    ASSERT_STR_EQ(bsErrorGet(options), "Undefined function \"max\"");
    bsErrorClear(options);
    bsExprFree(expr);

    /* A locals object */
    expr = bsParseExpression("a + b", 5, 0, NULL, false, NULL);
    BSValue locals = bsObjectNew();
    bsObjectSet(locals, "a", bsNumber(1));
    bsObjectSet(options->globals, "b", bsNumber(2));
    ASSERT_VALUE(bsEvaluateExpression(expr, options, locals, false), "3");
    bsRelease(locals);
    bsExprFree(expr);

    /* A function value in the locals object */
    bsLibraryGlobals(options->globals);
    expr = bsParseExpression("f(-3)", 5, 0, NULL, false, NULL);
    locals = bsObjectNew();
    bsObjectSet(locals, "f", bsRetain(bsObjectGet(options->globals, "mathAbs")));
    ASSERT_VALUE(bsEvaluateExpression(expr, options, locals, false), "3");
    bsRelease(locals);
    bsExprFree(expr);

    /* A locals object that does not contain the function falls through to globals */
    expr = bsParseExpression("mathAbs(-5)", 11, 0, NULL, false, NULL);
    locals = bsObjectNew();
    bsObjectSet(locals, "other", bsNumber(1));
    ASSERT_VALUE(bsEvaluateExpression(expr, options, locals, false), "5");
    bsRelease(locals);
    bsExprFree(expr);

    /* A function call with no globals object */
    expr = bsParseExpression("mathAbs(-1)", 11, 0, NULL, false, NULL);
    BSValue savedGlobals = options->globals;
    options->globals = bsNull();
    ASSERT_VALUE(bsEvaluateExpression(expr, options, bsNull(), false), "null");
    options->globals = savedGlobals;
    bsExprFree(expr);

    bsOptionsFree(options);
}


/* Decode an expression model and assert its evaluation's JSON */
static void bsTestEvalModel(const char *json, BSOptions *options, bool builtins, const char *expectedJSON)
{
    BSValue model = bsJSONDecode(json, strlen(json), NULL);
    ASSERT_VALUE(bsEvaluateExpressionModel(model, options, bsNull(), builtins), expectedJSON);
    bsRelease(model);
}


TEST(runtime_expression_model)
{
    BSOptions *options = bsTestOptions();

    /* Evaluate a model against a locals object */
    static const char *binaryModel = "{\"binary\":{\"op\":\"+\",\"left\":{\"number\":1},"
        "\"right\":{\"variable\":\"a\"}}}";
    BSValue exprModel = bsJSONDecode(binaryModel, strlen(binaryModel), NULL);
    BSValue locals = bsObjectNew();
    bsObjectSet(locals, "a", bsNumber(2));
    ASSERT_VALUE(bsEvaluateExpressionModel(exprModel, options, locals, false), "3");
    bsRelease(exprModel);
    bsRelease(locals);

    /* Special variables */
    bsTestEvalModel("{\"variable\":\"true\"}", options, false, "true");
    bsTestEvalModel("{\"variable\":\"false\"}", options, false, "false");
    bsTestEvalModel("{\"variable\":\"null\"}", options, false, "null");

    /* Groups and the built-in "if" */
    bsTestEvalModel("{\"group\":{\"number\":1}}", options, false, "1");
    bsTestEvalModel("{\"function\":{\"name\":\"if\",\"args\":[{\"variable\":\"true\"},"
                    "{\"number\":1},{\"number\":2}]}}", options, false, "1");
    bsTestEvalModel("{\"function\":{\"name\":\"if\",\"args\":[]}}", options, false, "null");
    bsTestEvalModel("{\"function\":{\"name\":\"if\",\"args\":[{\"variable\":\"false\"}]}}", options, false,
                    "null");
    bsTestEvalModel("{\"function\":{\"name\":\"if\",\"args\":[{\"variable\":\"true\"},{\"number\":7}]}}",
                    options, false, "7");
    bsTestEvalModel("{\"function\":{\"name\":\"mathAbs\"}}", options, true, "null");
    bsOptionsFree(options);
}


TEST(runtime_expression_model_invalid)
{
    static const char *invalid[] = {
        "1",
        "{}",
        "{\"function\":{}}",
        "{\"function\":{\"name\":\"f\",\"args\":[1]}}",
        "{\"binary\":{}}",
        "{\"binary\":{\"op\":\"?\",\"left\":{\"number\":1},\"right\":{\"number\":1}}}",
        "{\"binary\":{\"op\":\"+\",\"left\":1,\"right\":{\"number\":1}}}",
        "{\"binary\":{\"op\":\"+\",\"left\":{\"number\":1},\"right\":1}}",
        "{\"unary\":{}}",
        "{\"unary\":{\"op\":\"?\",\"expr\":{\"number\":1}}}",
        "{\"unary\":{\"op\":\"-\",\"expr\":1}}",
        "{\"group\":1}",
        "{\"function\":{\"name\":\"if\",\"args\":[1]}}",
        "{\"function\":{\"name\":\"if\",\"args\":[{\"number\":1},1]}}",
        "{\"function\":{\"name\":\"if\",\"args\":[{\"number\":1},{\"number\":2},1]}}",
        "{\"binary\":{\"op\":\"&&\",\"left\":1,\"right\":{\"number\":1}}}",
        "{\"binary\":{\"op\":\"&&\",\"left\":{\"number\":1},\"right\":1}}",
        "{\"binary\":{\"op\":\"||\",\"left\":1,\"right\":{\"number\":1}}}",
        "{\"binary\":{\"op\":\"||\",\"left\":{\"number\":1},\"right\":1}}"
    };
    for (size_t ix = 0; ix < sizeof(invalid) / sizeof(invalid[0]); ix++) {
        BSValue model = bsJSONDecode(invalid[ix], strlen(invalid[ix]), NULL);
        ASSERT_NULL(bsExprFromModel(model));
        bsRelease(model);
    }

    /* An invalid model evaluates to null */
    BSOptions *options = bsTestOptions();
    BSValue model = bsObjectNew();
    ASSERT_VALUE(bsEvaluateExpressionModel(model, options, bsNull(), false), "null");
    bsRelease(model);
    bsOptionsFree(options);
}


TEST(runtime_script_model)
{
    static const char *scriptText = "include <a.bare>\ninclude 'b.bare'\nfunction f(a...):\n"
        "    return a\nendfunction\nasync function g():\nendfunction\n"
        "x = 1\nlabel:\njump label\nreturn x";
    BSScript *script = bsParseScript(scriptText, strlen(scriptText), 1, "s.bare", NULL);
    ASSERT_NOT_NULL(script);
    script->system = true;
    BSValue model = bsScriptToModel(script);
    ASSERT_TRUE(bsObjectHas(model, "statements"));
    ASSERT_TRUE(bsObjectHas(model, "scriptName"));
    ASSERT_TRUE(bsObjectHas(model, "scriptLines"));
    ASSERT_TRUE(bsObjectHas(model, "system"));
    bsRelease(model);
    bsScriptRelease(script);
}


TEST(runtime_globals)
{
    /* Globals set before execution are visible to the script */
    BSOptions *options = bsTestOptions();
    bsObjectSet(options->globals, "vName", bsStringNew("World"));
    ASSERT_VALUE(bsTestExecuteOptions("return 'Hello, ' + vName", options), "\"Hello, World\"");
    bsOptionsFree(options);
}


/* The largest batch the fetch function double has seen, and its requests in all */
static size_t bsTestFetchBatchMax;
static size_t bsTestFetchRequests;

static void bsTestFetchFn(const BSFetchRequest *requests, BSValue *responses, size_t count, void *data)
{
    if (count > bsTestFetchBatchMax) {
        bsTestFetchBatchMax = count;
    }
    bsTestFetchRequests += count;
    for (size_t ix = 0; ix < count; ix++) {
        const char *url = requests[ix].url;
        const char *text = "includedGlobal = 'included'\nfunction includedFn():\n    return 'from include'\n"
            "endfunction";
        if (strcmp(url, "fail.bare") == 0) {
            text = NULL;
        } else if (strcmp(url, "lint.bare") == 0) {
            text = "function lintFn():\n    unused = 1\n    return 1\nendfunction";
        } else if (strcmp(url, "bad.bare") == 0) {
            text = "a = 1 +";
        } else if (strcmp(url, "error.bare") == 0) {
            text = "undefinedFunc()";
        } else if (strcmp(url, "nested.bare") == 0) {
            text = "include 'other.bare'";
        } else if (strcmp(url, "count.bare") == 0) {
            text = "includeCount = systemGlobalGet('includeCount', 0) + 1";
        } else if (strcmp(url, "nestedCount.bare") == 0) {
            text = "include 'count.bare'";
        }
        if (text != NULL) {
            responses[ix] = bsStringNew(text);
        }
    }
}


/* Options with the test fetch function */
static BSOptions *bsTestFetchOptions(void)
{
    BSOptions *options = bsTestOptions();
    options->fetchFn = bsTestFetchFn;
    return options;
}


TEST(runtime_includes)
{
    BSOptions *options = bsTestFetchOptions();
    ASSERT_VALUE(bsTestExecuteOptions("include 'a.bare'\nreturn [includedGlobal, includedFn()]", options),
                 "[\"included\",\"from include\"]");

    /* Including the same file again is a no-op */
    ASSERT_VALUE(bsTestExecuteOptions("include 'a.bare'\nreturn includedGlobal", options), "\"included\"");
    bsOptionsFree(options);

    /* A nested include resolves relative to its includer */
    options = bsTestFetchOptions();
    ASSERT_VALUE(bsTestExecuteOptions("include 'nested.bare'\nreturn includedGlobal", options), "\"included\"");
    bsOptionsFree(options);

    /* A failed include */
    options = bsTestFetchOptions();
    ASSERT_VALUE(bsTestExecuteOptions("include 'fail.bare'", options), "null");
    ASSERT_STR_EQ(bsTestErrorText(), "test.bare:1: Include of \"fail.bare\" failed");
    bsOptionsFree(options);

    /* An include statement's includes fetch as one batch, then execute in order */
    options = bsTestFetchOptions();
    bsTestFetchBatchMax = 0;
    ASSERT_VALUE(bsTestExecuteOptions("include 'a.bare'\ninclude 'count.bare'\ninclude 'b.bare'\n"
                                      "return [includedGlobal, includeCount]", options),
                 "[\"included\",1]");
    ASSERT_INT_EQ(bsTestFetchBatchMax, 3);
    ASSERT_VALUE(bsTestExecuteOptions("include 'a.bare'\ninclude 'fail.bare'\ninclude 'b.bare'", options), "null");
    ASSERT_STR_EQ(bsTestErrorText(), "test.bare:1: Include of \"fail.bare\" failed");
    bsOptionsFree(options);

    /* An include that an earlier include of its batch included itself executes once, from the text
       the batch fetched - it is not fetched again */
    options = bsTestFetchOptions();
    bsTestFetchRequests = 0;
    ASSERT_VALUE(bsTestExecuteOptions("include 'nestedCount.bare'\ninclude 'count.bare'\nreturn includeCount",
                                      options), "1");
    ASSERT_INT_EQ(bsTestFetchRequests, 2);
    bsOptionsFree(options);

    /* An include repeated in a statement is fetched once */
    options = bsTestFetchOptions();
    bsTestFetchRequests = 0;
    ASSERT_VALUE(bsTestExecuteOptions("include 'a.bare'\ninclude 'a.bare'\nreturn includedGlobal", options),
                 "\"included\"");
    ASSERT_INT_EQ(bsTestFetchRequests, 1);
    bsOptionsFree(options);

    /* No fetch function */
    options = bsTestOptions();
    ASSERT_VALUE(bsTestExecuteOptions("include 'a.bare'", options), "null");
    ASSERT_STR_EQ(bsTestErrorText(), "test.bare:1: Include of \"a.bare\" failed");
    bsOptionsFree(options);

    /* An include that fails to parse */
    options = bsTestFetchOptions();
    ASSERT_VALUE(bsTestExecuteOptions("include 'bad.bare'", options), "null");
    ASSERT_STR_EQ(bsTestErrorText(), "bad.bare:1: Syntax error\na = 1 +\n       ^\n");
    bsOptionsFree(options);

    /* An include with a runtime error */
    options = bsTestFetchOptions();
    ASSERT_VALUE(bsTestExecuteOptions("include 'error.bare'", options), "null");
    ASSERT_STR_EQ(bsTestErrorText(), "error.bare:1: Undefined function \"undefinedFunc\"");
    bsOptionsFree(options);

    /* An include with a URL function */
    options = bsTestFetchOptions();
    options->urlFn = bsUrlFileRelative;
    options->urlData = strdup("dir/script.bare");
    options->urlDataFree = free;
    ASSERT_VALUE(bsTestExecuteOptions("include 'a.bare'\nreturn includedGlobal", options), "\"included\"");
    bsOptionsFree(options);
}


TEST(runtime_system_includes)
{
    bsSystemIncludeClear();
    ASSERT_NULL(bsSystemIncludeGet("nope.bare"));

    bsSystemIncludeRegister("sys.bare", "systemGlobal = 'system'");
    ASSERT_STR_EQ(bsSystemIncludeGet("sys.bare"), "systemGlobal = 'system'");

    BSOptions *options = bsTestOptions();
    ASSERT_VALUE(bsTestExecuteOptions("include <sys.bare>\nreturn systemGlobal", options), "\"system\"");
    /* A repeated system include is a no-op */
    ASSERT_VALUE(bsTestExecuteOptions("include <sys.bare>\nreturn systemGlobal", options), "\"system\"");
    bsOptionsFree(options);

    /* A missing system include */
    options = bsTestOptions();
    ASSERT_VALUE(bsTestExecuteOptions("include <nope.bare>", options), "null");
    ASSERT_STR_EQ(bsTestErrorText(), "test.bare:1: Include of \"nope.bare\" failed");
    bsOptionsFree(options);

    /* The system include search path */
    const char *directory = bsTestTempDir();
    bsTestTempFile("path.bare", "pathGlobal = 'from path'");
    bsSystemIncludePath("nonexistent-directory");
    bsSystemIncludePath(directory);
    ASSERT_STR_EQ(bsSystemIncludeGet("path.bare"), "pathGlobal = 'from path'");
    ASSERT_NULL(bsSystemIncludeGet("missing.bare"));

    options = bsTestOptions();
    ASSERT_VALUE(bsTestExecuteOptions("include <path.bare>\nreturn pathGlobal", options), "\"from path\"");
    bsOptionsFree(options);
    bsSystemIncludeClear();
}


/* Options with a coverage object installed; "*coverage" receives a borrowed reference to it */
static BSOptions *bsTestCoverageOptions(BSValue *coverage, bool enabled)
{
    BSOptions *options = bsTestOptions();
    *coverage = bsObjectNew();
    bsObjectSet(*coverage, "enabled", bsBoolean(enabled));
    bsObjectSet(options->globals, BS_GLOBAL_COVERAGE, *coverage);
    return options;
}


/* A script's covered-lines object (borrowed) */
static BSValue bsTestCovered(BSValue coverage, const char *script)
{
    return bsObjectGet(bsObjectGet(bsObjectGet(coverage, "scripts"), script), "covered");
}

/* A covered line's hit count */
static double bsTestCoveredCount(BSValue coverage, const char *script, const char *line)
{
    return bsObjectGet(bsObjectGet(bsTestCovered(coverage, script), line), "count").u.number;
}


TEST(runtime_fused_slots)
{
    /* Each fused slot pair, with set slots and with unset slots that fall through to the globals */
    static const char *text =
        "g = 10\n"
        "h = 20\n"
        "function pairs(a, b):\n"
        "    c = a + b\n"
        "    d = c + 1\n"
        "    r = [c, d]\n"
        "    if a:\n"
        "        arrayPush(r, 'a')\n"
        "    endif\n"
        "    if !b:\n"
        "        arrayPush(r, 'notb')\n"
        "    endif\n"
        "    while b:\n"
        "        b = 0\n"
        "        arrayPush(r, 'b')\n"
        "    endwhile\n"
        "    return r\n"
        "endfunction\n"
        "function unset(flag):\n"
        "    if g:\n"
        "        r = [g, h]\n"
        "        r = arrayPush(r, g + 1)\n"
        "        s = g\n"
        "        arrayPush(r, s)\n"
        "    endif\n"
        "    if flag:\n"
        "        g = 1\n"
        "        h = 2\n"
        "        s = 3\n"
        "        r = null\n"
        "    endif\n"
        "    return r\n"
        "endfunction\n"
        "return [pairs(1, 2), pairs(0, 0), unset(false)]";
    ASSERT_VALUE(bsTestExecute(text), "[[3,4,\"a\",\"b\"],[0,1,\"notb\"],[10,20,11,10]]");
}


TEST(runtime_coverage_forgotten_model)
{
    /* A script that forgot its model borrows the statement models back when coverage records them */
    BSValue coverage;
    BSOptions *options = bsTestCoverageOptions(&coverage, true);
    static const char *text = "a = 1\nfunction f(x):\n    return x + a\nendfunction\nreturn f(2)";
    BSScript *script = bsParseScript(text, strlen(text), 1, "forget.bare", NULL);
    ASSERT_NOT_NULL(script);
    bsScriptForgetModel(script);
    ASSERT_NULL(script->code.cover);
    ASSERT_NULL(script->functions[0]->code.cover);
    ASSERT_VALUE(bsExecuteScript(script, options), "3");
    ASSERT_DOUBLE_EQ(bsTestCoveredCount(coverage, "forget.bare", "1"), 1);
    ASSERT_DOUBLE_EQ(bsTestCoveredCount(coverage, "forget.bare", "3"), 1);
    BSValue covered = bsTestCovered(coverage, "forget.bare");
    BSValue statement = bsObjectGet(bsObjectGet(covered, "3"), "statement");
    ASSERT_INT_EQ(statement.type, BS_OBJECT);
    ASSERT_TRUE(bsObjectHas(statement, "return"));
    ASSERT_NOT_NULL(script->code.cover);
    ASSERT_NOT_NULL(script->functions[0]->code.cover);
    ASSERT_INT_EQ(script->model.type, BS_OBJECT);
    bsScriptRelease(script);
    bsOptionsFree(options);
}


TEST(runtime_coverage_forgotten_model_mismatch)
{
    /* A forgotten model whose script lines parse to different chunks stays without statement models */
    BSValue coverage;
    BSOptions *options = bsTestCoverageOptions(&coverage, true);
    static const char *json = "{\"statements\":[{\"expr\":{\"name\":\"a\",\"expr\":{\"number\":1},\"lineNumber\":1}}],"
        "\"scriptLines\":[\"a = 1\",\"b = 2\"]}";
    BSScript *script = bsTestScriptFromJSON(json, "mismatch.bare");
    ASSERT_NOT_NULL(script);
    bsScriptForgetModel(script);
    bsRelease(bsExecuteScript(script, options));
    ASSERT_NULL(script->code.cover);
    bsScriptRelease(script);
    bsOptionsFree(options);
}


TEST(runtime_coverage)
{
    BSValue coverage;
    BSOptions *options = bsTestCoverageOptions(&coverage, true);
    ASSERT_VALUE(bsTestExecuteOptions("a = 1\nif a:\n    a = 2\nendif\nreturn a", options), "2");
    ASSERT_DOUBLE_EQ(bsTestCoveredCount(coverage, "test.bare", "1"), 1);

    /* A second parse of the same name increments the existing covered line */
    ASSERT_VALUE(bsTestExecuteOptions("a = 1\nif a:\n    a = 2\nendif\nreturn a", options), "2");
    ASSERT_DOUBLE_EQ(bsTestCoveredCount(coverage, "test.bare", "1"), 2);
    bsOptionsFree(options);

    /* Coverage is not recorded for system scripts */
    options = bsTestCoverageOptions(&coverage, true);
    bsSystemIncludeRegister("cov.bare", "x = 1");
    ASSERT_VALUE(bsTestExecuteOptions("include <cov.bare>\nreturn x", options), "1");
    ASSERT_FALSE(bsObjectHas(bsObjectGet(coverage, "scripts"), "cov.bare"));
    bsOptionsFree(options);
    bsSystemIncludeClear();

    /* Coverage is not recorded for a script with no name */
    options = bsTestCoverageOptions(&coverage, true);
    BSScript *script = bsParseScript("a = 1", 5, 1, NULL, NULL);
    bsRelease(bsExecuteScript(script, options));
    ASSERT_FALSE(bsObjectHas(coverage, "scripts"));
    bsScriptRelease(script);
    bsOptionsFree(options);

    /* Coverage disabled */
    options = bsTestCoverageOptions(&coverage, false);
    ASSERT_VALUE(bsTestExecuteOptions("return 1", options), "1");
    ASSERT_FALSE(bsObjectHas(coverage, "scripts"));
    bsOptionsFree(options);
}


TEST(runtime_coverage_jump)
{
    /* A jump records the label statement's coverage */
    BSValue coverage;
    BSOptions *options = bsTestCoverageOptions(&coverage, true);
    ASSERT_VALUE(bsTestExecuteOptions("i = 0\nwhile i < 3:\n    i = i + 1\nendwhile\nreturn i", options), "3");
    ASSERT_TRUE(bsObjectCount(bsTestCovered(coverage, "test.bare")) >= 4);
    ASSERT_TRUE(bsTestCoveredCount(coverage, "test.bare", "3") >= 3);
    bsOptionsFree(options);

    /* An expression jump (if/&&) is not a label; coverage still records the statement */
    options = bsTestCoverageOptions(&coverage, true);
    ASSERT_VALUE(bsTestExecuteOptions("a = if(false, 1, 2)\nreturn a || 0", options), "2");
    bsOptionsFree(options);
}


TEST(runtime_coverage_cache)
{
    /* Reusing a script with a new coverage object resets the line-index cache */
    BSScript *script = bsParseScript("a = 1\nreturn a", strlen("a = 1\nreturn a"), 1, "cache.bare", NULL);
    BSValue coverage;
    BSOptions *options = bsTestCoverageOptions(&coverage, true);
    bsRelease(bsExecuteScript(script, options));
    ASSERT_DOUBLE_EQ(bsTestCoveredCount(coverage, "cache.bare", "1"), 1);

    BSValue coverage2 = bsObjectNew();
    bsObjectSet(coverage2, "enabled", bsBoolean(true));
    bsObjectSet(options->globals, BS_GLOBAL_COVERAGE, coverage2);
    bsRelease(bsExecuteScript(script, options));
    ASSERT_DOUBLE_EQ(bsTestCoveredCount(coverage2, "cache.bare", "1"), 1);
    bsScriptRelease(script);
    bsOptionsFree(options);

    /* A high line number grows the line-index array past its initial cap */
    options = bsTestCoverageOptions(&coverage, true);
    static const char *highModel =
        "{\"scriptName\":\"high.bare\",\"statements\":[{\"expr\":{\"name\":\"a\","
        "\"expr\":{\"number\":1},\"lineNumber\":40}}]}";
    script = bsTestScriptFromJSON(highModel, "high.bare");
    bsRelease(bsExecuteScript(script, options));
    ASSERT_DOUBLE_EQ(bsTestCoveredCount(coverage, "high.bare", "40"), 1);
    bsScriptRelease(script);
    bsOptionsFree(options);

    /* A statement at line 0 is not recorded */
    options = bsTestCoverageOptions(&coverage, true);
    static const char *zeroModel =
        "{\"scriptName\":\"zero.bare\",\"statements\":[{\"expr\":{\"name\":\"a\","
        "\"expr\":{\"number\":1}}}]}";
    script = bsTestScriptFromJSON(zeroModel, "zero.bare");
    bsRelease(bsExecuteScript(script, options));
    ASSERT_FALSE(bsObjectHas(coverage, "scripts"));
    bsScriptRelease(script);
    bsOptionsFree(options);
}


TEST(runtime_eval_borrow)
{
    /* A later operand can reassign a borrowed string; the left side keeps the original */
    ASSERT_VALUE(bsTestExecute(
        "function mutate():\n    s = 'b'\n    return 'x'\nendfunction\n"
        "s = 'a'\nreturn s + mutate()"), "\"ax\"");
    ASSERT_VALUE(bsTestExecute(
        "function mutate():\n    s = 'b'\n    return 'x'\nendfunction\n"
        "s = 'a'\nreturn s + (mutate())"), "\"ax\"");
    ASSERT_VALUE(bsTestExecute(
        "function mutate():\n    s = 'b'\n    return 'z'\nendfunction\n"
        "s = 'a'\nreturn s + (mutate() + '!')"), "\"az!\"");
    ASSERT_VALUE(bsTestExecute(
        "function mutate():\n    s = 'b'\n    return 'z'\nendfunction\n"
        "s = 'a'\nt = 'y'\nreturn s + (t + mutate())"), "\"ayz\"");

    /* Pure right-hand sides keep the left side borrowed */
    ASSERT_VALUE(bsTestExecute("s = 'a'\nt = 'b'\nreturn s + t"), "\"ab\"");
    ASSERT_VALUE(bsTestExecute("s = 'a'\nt = 'b'\nreturn s + (t)"), "\"ab\"");
    ASSERT_VALUE(bsTestExecute("s = 'a'\nt = 'b'\nu = 'c'\nreturn s + (t + u)"), "\"abc\"");
    ASSERT_VALUE(bsTestExecute("s = 'a'\nreturn s + (!false)"), "\"atrue\"");

    /* Function arguments keep a borrowed heap value unless a later arg is effectful */
    ASSERT_VALUE(bsTestExecute(
        "function g(x, y):\n    return x + y\nendfunction\n"
        "function mutate():\n    s = 'b'\n    return 'x'\nendfunction\n"
        "s = 'a'\nreturn g(s, mutate())"), "\"ax\"");
    ASSERT_VALUE(bsTestExecute(
        "function g(x, y):\n    return x + y\nendfunction\n"
        "s = 'a'\nreturn g(s, 'z')"), "\"az\"");

    /* A call with many arguments */
    ASSERT_VALUE(bsTestExecute(
        "function g(a, b, c, d, e, f, g, h, i, j):\n    return a + j\nendfunction\n"
        "return g(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)"), "11");
    ASSERT_VALUE(bsTestExecute(
        "function g(a, b, c, d, e, f, g, h, i, j):\n    return a + j\nendfunction\n"
        "function mutate():\n    s = 'b'\n    return 'z'\nendfunction\n"
        "s = 'a'\nreturn g(s, 1, 2, 3, 4, 5, 6, 7, 8, mutate())"), "\"az\"");
    ASSERT_VALUE(bsTestExecute(
        "function g(a, b, c, d, e, f, g, h, i, j):\n    return i + j\nendfunction\n"
        "function mutate():\n    s = 'b'\n    return 'z'\nendfunction\n"
        "s = 'a'\nreturn g(1, 2, 3, 4, 5, 6, 7, 8, s, mutate())"), "\"az\"");
}


TEST(runtime_call_non_function_debug)
{
    /* Calling a non-function value logs in debug mode and evaluates to null */
    BSOptions *options = bsTestOptions();
    options->debug = true;
    ASSERT_VALUE(bsTestExecuteOptions("x = 1\nreturn x()", options), "null");
    ASSERT_STR_EQ(bsTestLogText(),
                  "BareScript: Function \"x\" failed with error: not a function\n");
    bsOptionsFree(options);
}


TEST(runtime_duplicate_arguments)
{
    /* A repeated argument name is the last argument's, as in the references; each argument still fills a slot */
    ASSERT_VALUE(bsTestExecute("function f(a, a):\n    return a\nendfunction\nreturn f(1, 2)"), "2");
    ASSERT_VALUE(bsTestExecute("function f(a, b, a):\n    return [a, b]\nendfunction\nreturn f(1, 2, 3)"), "[3,2]");
}


TEST(runtime_many_arguments)
{
    /* A function with many arguments */
    ASSERT_VALUE(bsTestExecute("function g(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11):\n"
                               "    return a11\nendfunction\nreturn g(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11)"), "11");

    /* Deeply nested calls with many arguments */
    ASSERT_VALUE(bsTestExecute(
        "function g(a, b, c, d, e, f, g, h, i):\n    return i\nendfunction\n"
        "return g(null, null, null, null, null, null, null, null, null)"), "null");
    ASSERT_VALUE(bsTestExecute(
        "function g(a, b, c, d, e, f, g, h, i):\n    return i\nendfunction\n"
        "return g(true, true, true, true, true, true, true, true, true)"), "true");
    ASSERT_VALUE(bsTestExecute(
        "function g(a, b, c, d, e, f, g, h):\n    return h\nendfunction\n"
        "a = 1\nb = 0\nreturn g(1, 2, 3, 4, 5, 6, 7, a && b)"), "0");
    ASSERT_VALUE(bsTestExecute(
        "function h():\n    return 9\nendfunction\n"
        "function g(a, b, c, d, e, f, g, h, i):\n    return i\nendfunction\n"
        "return g(1, 2, 3, 4, 5, 6, 7, 8, h())"), "9");

    /* An unassigned function local falls through to the globals */
    ASSERT_VALUE(bsTestExecute(
        "x = 5\n"
        "function f():\n    x = x + 1\n    return x\nendfunction\n"
        "return f()"), "6");
}


TEST(runtime_string_append)
{
    /* "s = s + x" on a function local holding the string's only reference appends in place: within
       the recycled block's room, then growing it out of its size class, then doubling repeatedly. A
       global loads into a temporary first, so it never does. */
    ASSERT_VALUE(bsTestExecute(
        "function f():\n"
        "    s = stringSlice('abc', 0, 1)\n"
        "    i = 0\n    while i < 40:\n        s = s + 'xy'\n        i = i + 1\n    endwhile\n"
        "    return [stringLength(s), stringSlice(s, 0, 5), stringSlice(s, 79)]\n"
        "endfunction\nreturn f()"), "[81,\"axyxy\",\"xy\"]");
    ASSERT_VALUE(bsTestExecute(
        "function f():\n"
        "    s = stringSlice('abc', 0, 1)\n    s = s + stringRepeat('z', 300)\n"
        "    return [stringLength(s), stringSlice(s, 299)]\n"
        "endfunction\nreturn f()"), "[301,\"zz\"]");

    /* The right operand's string representation, and the code point length and index of a
       non-ASCII string */
    ASSERT_VALUE(bsTestExecute(
        "function f():\n"
        "    s = stringSlice('abc', 0, 1)\n    s = s + 1.5\n    s = s + true\n    s = s + null\n    return s\n"
        "endfunction\nreturn f()"), "\"a1.5truenull\"");
    ASSERT_VALUE(bsTestExecute(
        "function f():\n"
        "    s = stringSlice('\xc3\xa9\xc3\xa9x', 0, 2)\n    c = stringCharAt(s, 1)\n    s = s + '\xc3\xa9'\n"
        "    return [stringLength(s), s, c, stringCharAt(s, 2)]\n"
        "endfunction\nreturn f()"), "[3,\"\xc3\xa9\xc3\xa9\xc3\xa9\",\"\xc3\xa9\",\"\xc3\xa9\"]");

    /* Not in place: a string appended to itself, one another variable shares, a literal, a global */
    ASSERT_VALUE(bsTestExecute(
        "function f():\n    s = stringSlice('abc', 0, 2)\n    s = s + s\n    return s\nendfunction\nreturn f()"),
        "\"abab\"");
    ASSERT_VALUE(bsTestExecute(
        "function f():\n    s = stringSlice('abc', 0, 1)\n    t = s\n    s = s + 'q'\n    return t + s\n"
        "endfunction\nreturn f()"), "\"aaq\"");
    ASSERT_VALUE(bsTestExecute("function f():\n    s = 'abc'\n    s = s + 'd'\n    return s\nendfunction\nreturn f()"),
                 "\"abcd\"");
    ASSERT_VALUE(bsTestExecute("s = stringSlice('abc', 0, 1)\ns = s + 'd'\nreturn s"), "\"ad\"");
}


TEST(runtime_concat_all_types)
{
    /* String concatenation formats every value type */
    ASSERT_VALUE(bsTestExecute("return 'a' + [1, 2]"), "\"a[1,2]\"");
    ASSERT_VALUE(bsTestExecute("return 'a' + {'k': 1}"), "\"a{\\\"k\\\":1}\"");
    ASSERT_VALUE(bsTestExecute("return 'a' + systemType"), "\"a<function>\"");
    ASSERT_VALUE(bsTestExecute("return 'a' + regexNew('x')"), "\"a<regex>\"");
    ASSERT_VALUE(bsTestExecute("return [1, 2] + 'a'"), "\"[1,2]a\"");
    ASSERT_VALUE(bsTestExecute("return 'a' + true + false + null + 1"), "\"atruefalsenull1\"");
    ASSERT_VALUE(bsTestExecute("return stringLength('a' + datetimeNew(2026, 8, 6)) > 20"), "true");

    /* arrayJoin formats every value type through the same path */
    ASSERT_VALUE(bsTestExecute("return arrayJoin([1, true, null, [2], {'k': 1}, systemType], '|')"),
                 "\"1|true|null|[2]|{\\\"k\\\":1}|<function>\"");
    ASSERT_VALUE(bsTestExecute("return arrayJoin([regexNew('x')], '|')"), "\"<regex>\"");
}


TEST(runtime_include_lint_debug)
{
    /* In debug mode the runtime lints each include it loads */
    BSOptions *options = bsTestFetchOptions();
    options->debug = true;
    bsRelease(bsTestExecuteOptions("include 'lint.bare'", options));
    ASSERT_STR_CONTAINS(bsTestLogText(), "BareScript: Include \"lint.bare\" static analysis...");
    ASSERT_STR_CONTAINS(bsTestLogText(), "Unused variable");
    bsOptionsFree(options);

    /* An include with no warnings logs nothing */
    options = bsTestFetchOptions();
    options->debug = true;
    bsRelease(bsTestExecuteOptions("include 'a.bare'", options));
    ASSERT_STR_NOT_CONTAINS(bsTestLogText(), "static analysis");
    bsOptionsFree(options);
}


TEST(runtime_include_bundled_model)
{
    /* A registered system include whose text is an invalid JSON model fails */
    bsSystemIncludeRegister("badmodel.bare", "{\"statements\":[{}]}");
    ASSERT_VALUE(bsTestExecute("include <badmodel.bare>"), "null");
    ASSERT_STR_EQ(bsTestErrorText(), "test.bare:1: Include of \"badmodel.bare\" failed");
    bsSystemIncludeClear();

    /* A registered system include whose text is a valid JSON model executes it */
    bsSystemIncludeRegister("model.bare",
                            "{\"statements\":[{\"expr\":{\"name\":\"modelGlobal\",\"expr\":{\"string\":\"from model\"}}}]}");
    ASSERT_VALUE(bsTestExecute("include <model.bare>\nreturn modelGlobal"), "\"from model\"");
    bsSystemIncludeClear();

    /* A registered system include whose text is source is parsed */
    bsSystemIncludeRegister("source.bare", "sourceGlobal = 'parsed'");
    ASSERT_VALUE(bsTestExecute("include <source.bare>\nreturn sourceGlobal"), "\"parsed\"");
    bsSystemIncludeClear();
}


TEST(runtime_function_error)
{
    /* The first function error wins, as the first runtime error does */
    BSOptions *options = bsOptionsNew();
    bsFunctionError(options, "first %d", 1);
    bsFunctionError(options, "second %d", 2);
    ASSERT_VALUE_STRING_KEEP(options->argsError, "first 1");
    bsOptionsFree(options);

    /* A NULL options is a no-op */
    bsFunctionError(NULL, "ignored");
}


TEST(runtime_intrinsic_opcodes)
{
    /* A global call to one of the intrinsics by name, with its happy-path argument count, runs in
       the interpreter; each shape that is not the happy path takes the general call */
    ASSERT_VALUE(bsTestExecute(
        "a = [1, 2]\no = {'k': 3}\n"
        "arrayPush(a, 4)\narraySet(a, 0, 9)\nobjectSet(o, 'j', 5)\n"
        "return [arrayGet(a, 2), arrayLength(a), arrayPush(a, 7), arraySet(a, 1, 8), objectGet(o, 'k'), "
        "objectGet(o, 'z', 6), objectGet(o, 'z'), objectSet(o, 'k', 1), stringLength('abc'), o, "
        "objectHas(o, 'k'), objectHas(o, 'z')]"),
        "[4,3,[9,8,4,7],8,3,6,null,1,3,{\"j\":5,\"k\":1},true,false]");

    /* Argument shapes the intrinsic does not take fall to the library function, which reports them */
    ASSERT_VALUE(bsTestExecute("return [arrayGet([1], 1), arrayGet('x', 0), arrayLength(1), objectGet(1, 'k'), "
                               "objectGet({}, 1), stringLength(1), arraySet([], 0, 1), objectSet(1, 'k', 1), arrayPush(1, 2), "
                               "objectHas(1, 'k'), objectHas({}, 1)]"),
                 "[null,null,0,null,null,0,null,null,null,false,false]");
    ASSERT_VALUE(bsTestExecute("return arrayGet([1], 0.5)"), "null");

    /* A cold site takes the general call, which resolves it; a warm site's handler sees the shapes
       itself. The first call warms every site with the happy path; each call after it misses one way. */
    ASSERT_VALUE(bsTestExecute(
        "function f(a, i, o, k, s, e):\n"
        "    stringSlice(s, i, e)\n"
        "    return [arrayGet(a, i), arrayLength(a), arrayPush(a, 2), arraySet(a, i, 1), objectGet(o, k), "
        "objectSet(o, k, 1), stringLength(s), stringSlice(s, i, e), stringSlice(s, i), objectHas(o, k), "
        "stringCharCodeAt(s, i)]\n"
        "endfunction\n"
        "f([1], 0, {'k': 3}, 'k', 'abc', 1)\n"
        "return [f([1], 1, {}, 'k', 'ab', 0), f('x', 'y', 1, 'k', 1, 'z'), f([1], 0.5, {}, 'k', 'abc', 'x'), "
        "f([1], 0, {}, 'k', 'abc', 4), f([1], 0, {}, 'k', 'abc', 'x'), f([1], 'y', {}, 'k', 'abc', 1), "
        "f([1], 1, {}, 'k', '\xc3\xa9x', 1), f([1], 3, {}, 'k', 'abc', 1)]"),
        "[[null,1,[1,1],1,null,1,2,\"\",\"b\",true,98],[null,0,null,null,null,null,0,null,null,false,null],"
        "[null,1,[1,2],null,null,1,3,null,null,true,null],[1,1,[1,2],1,null,1,3,null,\"abc\",true,97],"
        "[1,1,[1,2],1,null,1,3,null,\"abc\",true,97],[null,1,[1,2],null,null,1,3,null,null,true,null],"
        "[null,1,[1,1],1,null,1,2,\"\",\"x\",true,120],[null,1,[1,2],null,null,1,3,\"\",\"\",true,null]]");

    /* A script function of the same name shadows the intrinsic - the site's global is not the library's */
    ASSERT_VALUE(bsTestExecute(
        "function arrayGet(a, i):\n    return 'mine'\nendfunction\n"
        "function arrayLength(a):\n    return -1\nendfunction\n"
        "function arrayPush(a, v):\n    return -2\nendfunction\n"
        "function arraySet(a, i, v):\n    return -3\nendfunction\n"
        "function objectGet(o, k):\n    return -4\nendfunction\n"
        "function objectSet(o, k, v):\n    return -5\nendfunction\n"
        "function stringLength(s):\n    return -6\nendfunction\n"
        "function objectHas(o, k):\n    return -7\nendfunction\n"
        "function stringCharCodeAt(s, i):\n    return -8\nendfunction\n"
        "return [arrayGet([1], 0), arrayLength([]), arrayPush([], 1), arraySet([1], 0, 2), objectGet({}, 'k'), "
        "objectSet({}, 'k', 1), stringLength('abc'), objectHas({}, 'k'), stringCharCodeAt('a', 0)]"),
        "[\"mine\",-1,-2,-3,-4,-5,-6,-7,-8]");

    /* A locals object shadows too - an expression evaluated with locals takes the general call */
    ASSERT_VALUE(bsTestExecute(
        "function four(a):\n    return 4\nendfunction\n"
        "return barescriptEvaluateExpression({'function': {'name': 'arrayLength', 'args': [{'variable': 'a'}]}}, "
        "{'a': [1, 2, 3], 'arrayLength': four})"), "4");
    ASSERT_VALUE(bsTestExecute(
        "return barescriptEvaluateExpression({'function': {'name': 'arrayLength', 'args': [{'variable': 'a'}]}}, "
        "{'a': [1, 2, 3]})"), "3");

    /* stringSlice: two or three arguments, a null end, a reversed range, and the shapes that fall back */
    ASSERT_VALUE(bsTestExecute(
        "s = 'abcdef'\n"
        "return [stringSlice(s, 2), stringSlice(s, 1, 3), stringSlice(s, 1, null), stringSlice(s, 4, 2), "
        "stringSlice(s, 6), stringSlice(s, 0, 6), stringSlice('\xc3\xa9\xc3\xa9x', 1, 2)]"),
        "[\"cdef\",\"bc\",\"bcdef\",\"\",\"\",\"abcdef\",\"\xc3\xa9\"]");
    ASSERT_VALUE(bsTestExecute("return [stringSlice('abc', 4), stringSlice('abc', 0, 4), stringSlice('abc', 1.5), "
                               "stringSlice('abc', 0, 'x'), stringSlice(1, 0), stringSlice('abc', -1)]"),
                 "[null,null,null,null,null,null]");
    ASSERT_VALUE(bsTestExecute("stringSlice('abc', 1)\nreturn 1"), "1");
    ASSERT_VALUE(bsTestExecute("function stringSlice(s, b):\n    return 'mine'\nendfunction\nreturn stringSlice('abc', 1)"),
                 "\"mine\"");

    /* Called through a local holding the function, the library function itself runs */
    ASSERT_VALUE(bsTestExecute(
        "function g():\n    f = arrayGet\n    h = objectGet\n    k = stringSlice\n    c = stringCharCodeAt\n"
        "    return [f([1, 2], 1), h({'k': 3}, 'k'), h({}, 'k', 4), k('abcdef', 1, 3), c('abc', 1)]\n"
        "endfunction\nreturn g()"), "[2,3,4,\"bc\",98]");
}


TEST(runtime_compare_jumps)
{
    /* A jump on a comparison is one instruction on the comparison's operands: each comparison,
       taken and not, on numbers, strings, and null; a jump on true; a group; the conditional
       function; a while loop */
    ASSERT_VALUE(bsTestExecute(
        "function t(a, b):\n"
        "    r = []\n"
        "    if a == b:\n        arrayPush(r, 'eq')\n    endif\n"
        "    if a != b:\n        arrayPush(r, 'ne')\n    endif\n"
        "    if a < b:\n        arrayPush(r, 'lt')\n    endif\n"
        "    if a <= b:\n        arrayPush(r, 'le')\n    endif\n"
        "    if a > b:\n        arrayPush(r, 'gt')\n    endif\n"
        "    if a >= b:\n        arrayPush(r, 'ge')\n    endif\n"
        "    return arrayJoin(r, ',')\n"
        "endfunction\n"
        "return [t(1, 2), t(2, 2), t(3, 2), t('a', 'b'), t('b', 'b'), t(null, 1), t(1, null)]"),
        "[\"ne,lt,le\",\"eq,le,ge\",\"ne,gt,ge\",\"ne,lt,le\",\"eq,le,ge\",\"ne,lt,le\",\"ne,gt,ge\"]");
    ASSERT_VALUE(bsTestExecute("a = 1\njumpif (a < 2) yes\nreturn 'no'\nyes:\nreturn 'yes'"), "\"yes\"");
    ASSERT_VALUE(bsTestExecute("a = 1\njumpif ((a > 2)) yes\nreturn 'no'\nyes:\nreturn 'yes'"), "\"no\"");
    ASSERT_VALUE(bsTestExecute("return [if(1 < 2, 'a', 'b'), if(1 >= 2, 'a', 'b'), if((1 == 1), 'a')]"),
                 "[\"a\",\"b\",\"a\"]");
    ASSERT_VALUE(bsTestExecute("i = 0\nwhile i != 3:\n    i = i + 1\nendwhile\nreturn i"), "3");

    /* A comparison whose value is the result - a logical operator's left side - is still computed */
    ASSERT_VALUE(bsTestExecute("return [1 < 2 && 'x', 1 > 2 || 'y', 1 < 2 || 'z']"), "[\"x\",\"y\",true]");

    /* A logical condition jumps short-circuit: each comparison is one jump, and a "not" flips the
       sense - every combination, with the operand evaluation order and short-circuiting observable */
    ASSERT_VALUE(bsTestExecute(
        "log = []\n"
        "function t(v, name):\n"
        "    arrayPush(log, name)\n"
        "    return v\n"
        "endfunction\n"
        "function j(a, b, c):\n"
        "    arrayPush(log, '|')\n"
        "    jumpif (t(a, 'a') < 2 && t(b, 'b') < 2) yes\n"
        "    arrayPush(log, 'no')\n"
        "    jumpif (t(a, 'a') < 2 || t(b, 'b') < 2) yes2\n"
        "    arrayPush(log, 'no2')\n"
        "    jumpif (!(t(a, 'a') < 2 || t(b, 'b') < 2) && !(t(c, 'c') < 2)) yes3\n"
        "    arrayPush(log, 'no3')\n"
        "    jumpif (!(t(a, 'a') < 2 && t(b, 'b') < 2)) yes4\n"
        "    arrayPush(log, 'no4')\n"
        "    return\n"
        "    yes:\n"
        "    arrayPush(log, 'yes')\n"
        "    return\n"
        "    yes2:\n"
        "    arrayPush(log, 'yes2')\n"
        "    return\n"
        "    yes3:\n"
        "    arrayPush(log, 'yes3')\n"
        "    return\n"
        "    yes4:\n"
        "    arrayPush(log, 'yes4')\n"
        "endfunction\n"
        "j(1, 1, 1)\nj(1, 2, 1)\nj(2, 1, 1)\nj(2, 2, 1)\nj(2, 2, 2)\n"
        "return arrayJoin(log, ' ')"),
        "\"| a b yes | a b no a yes2 | a no a b yes2 | a no a b no2 a b c no3 a yes4 | a no a b no2 a b c yes3\"");

    /* Truthiness, a mixed operand, a chain of many, and the conditional function's condition */
    ASSERT_VALUE(bsTestExecute(
        "function f(a, b, c):\n"
        "    jumpif (a && b || c) yes\n"
        "    return 'no'\n"
        "    yes:\n"
        "    return 'yes'\n"
        "endfunction\n"
        "return [f(1, 1, 0), f(1, 0, 0), f(0, 0, 'c'), f(0, 1, null), f([], 1, 0), f([1], 1, 0)]"),
        "[\"yes\",\"no\",\"yes\",\"no\",\"no\",\"yes\"]");
    ASSERT_VALUE(bsTestExecute(
        "function f(n):\n"
        "    jumpif (n == 1 || n == 2 || n == 3 || n == 4 || n == 5 || n == 6 || n == 7 || n == 8 || n == 9 || n == 10 || "
        "n == 11 || n == 12 || n == 13 || n == 14 || n == 15 || n == 16 || n == 17 || n == 18) yes\n"
        "    return 'no'\n"
        "    yes:\n"
        "    return 'yes'\n"
        "endfunction\n"
        "return [f(1), f(10), f(18), f(19)]"), "[\"yes\",\"yes\",\"yes\",\"no\"]");
    ASSERT_VALUE(bsTestExecute(
        "function f(a, b):\n"
        "    return [if(a < 2 && b < 2, 'both', 'not'), if(a < 2 || b < 2, 'either', 'neither'), "
        "if(!(a < 2) && b, 'x', 'y'), if((a < 2) && (b < 2) || a == 3, 'p', 'q')]\n"
        "endfunction\n"
        "return [f(1, 1), f(1, 2), f(2, 1), f(3, 3)]"),
        "[[\"both\",\"either\",\"y\",\"p\"],[\"not\",\"either\",\"y\",\"q\"],[\"not\",\"either\",\"x\",\"q\"],"
        "[\"not\",\"neither\",\"x\",\"p\"]]");

    /* A comparison jump to a missing label errors only when taken */
    ASSERT_VALUE(bsTestExecute("jumpif (2 < 1) nowhere\nreturn 1"), "1");
    ASSERT_VALUE(bsTestExecute("jumpif (1 < 2) nowhere\nreturn 1"), "null");
    ASSERT_STR_EQ(bsTestErrorText(), "test.bare:1: Unknown jump label \"nowhere\"");

    /* A comparison whose operand is malformed is rejected like any other */
    ASSERT_VALUE(bsTestExecute("return barescriptEvaluateExpression("
                               "{'function': {'name': 'if', 'args': [{'binary': {'op': '<', 'left': {'bogus': 1}, 'right': {'number': 1}}}]}})"),
                 "null");

    /* Under coverage, a taken comparison jump records the label statement it lands on */
    BSValue coverage;
    BSOptions *options = bsTestCoverageOptions(&coverage, true);
    ASSERT_VALUE(bsTestExecuteOptions("i = 0\nloop:\ni = i + 1\njumpif (i < 3) loop\nreturn i", options), "3");
    ASSERT_TRUE(bsTestCoveredCount(coverage, "test.bare", "2") >= 3);
    bsOptionsFree(options);
}


TEST(runtime_intrinsic_string_char_code_at)
{
    /* An ASCII string's code point is read in place; a non-ASCII string or a bad index takes the library function */
    ASSERT_VALUE(bsTestExecute("return [stringCharCodeAt('abc', 1), stringCharCodeAt('\xc3\xa9x', 1), "
                               "stringCharCodeAt('\xc3\xa9x', 0), stringCharCodeAt('abc', 3), stringCharCodeAt('abc', 1.5)]"),
                 "[98,120,233,null,null]");
}
