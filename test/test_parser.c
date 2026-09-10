/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The parser unit tests
 */

#include <stdio.h>
#include <string.h>

#include "test.h"
#include "../src/internal.h"


/* Parse script text and return its model as JSON, or the parse error message */
static BSValue bsTestParse(const char *text)
{
    BSParserError error = {0};
    BSScript *script = bsParseScript(text, strlen(text), 1, "test.bare", &error);
    if (script == NULL) {
        BSValue message = bsRetain(error.message);
        bsParserErrorFree(&error);
        return message;
    }
    BSValue model = bsScriptToModel(script);
    bsObjectDelete(model, "scriptLines");
    BSValue json = bsJSONEncode(model, 0);
    bsRelease(model);
    bsScriptRelease(script);
    return json;
}


TEST(parser_forget_model)
{
    static const char *text = "a = 1\nfunction f(x):\n    return x + a\nendfunction\nreturn f(2)";
    BSScript *script = bsParseScript(text, strlen(text), 5, "forget.bare", NULL);
    ASSERT_NOT_NULL(script);
    BSValue model = bsScriptToModel(script);
    BSValue json = bsJSONEncode(model, 0);
    bsRelease(model);

    /* Forgetting the model leaves the lines, and bsScriptToModel re-parses them to the same model */
    bsScriptForgetModel(script);
    ASSERT_INT_EQ(bsValueType(script->model), BS_NULL);
    ASSERT_INT_EQ(script->startLineNumber, 5);
    BSValue reparsed = bsScriptToModel(script);
    ASSERT_INT_EQ(bsValueType(script->model), BS_NULL);
    BSValue reparsedJson = bsJSONEncode(reparsed, 0);
    ASSERT_STR_EQ(bsStringData(reparsedJson), bsStringData(json));
    ASSERT_STR_CONTAINS(bsStringData(reparsedJson), "\"lineNumber\":5");
    bsRelease(reparsed);
    bsRelease(reparsedJson);
    bsRelease(json);
    bsScriptRelease(script);
}


/* Assert that an owned string value contains "needle" and release it */
static void bsTestContainsValue(BSValue result, const char *needle)
{
    ASSERT_STR_CONTAINS(bsStringData(result), needle);
    bsRelease(result);
}


/* Assert that a script's parse result - its model JSON or its error message - contains "needle" */
static void bsTestParseContains(const char *text, const char *needle)
{
    bsTestContainsValue(bsTestParse(text), needle);
}


/* Parse expression text and return its model as JSON, or the parse error message */
static BSValue bsTestParseExprOpt(const char *text, bool arrayLiterals)
{
    BSParserError error = {0};
    BSExpr *expr = bsParseExpression(text, strlen(text), 0, NULL, arrayLiterals, &error);
    if (expr == NULL) {
        BSValue message = bsRetain(error.message);
        bsParserErrorFree(&error);
        return message;
    }
    BSValue model = bsExprToModel(expr);
    BSValue json = bsJSONEncode(model, 0);
    bsRelease(model);
    bsExprFree(expr);
    return json;
}


/* Parse expression text, without and with array literals */
static BSValue bsTestParseExpr(const char *text)
{
    return bsTestParseExprOpt(text, false);
}

static BSValue bsTestParseExprArray(const char *text)
{
    return bsTestParseExprOpt(text, true);
}


/* The expression form of bsTestParseContains */
static void bsTestParseExprContains(const char *text, const char *needle)
{
    bsTestContainsValue(bsTestParseExpr(text), needle);
}


TEST(parser_expression_literals)
{
    ASSERT_VALUE_STRING(bsTestParseExpr("5"), "{\"number\":5}");
    ASSERT_VALUE_STRING(bsTestParseExpr("-5"), "{\"number\":-5}");
    ASSERT_VALUE_STRING(bsTestParseExpr("+5"), "{\"number\":5}");
    ASSERT_VALUE_STRING(bsTestParseExpr("3.14"), "{\"number\":3.14}");
    ASSERT_VALUE_STRING(bsTestParseExpr("5."), "{\"number\":5}");
    ASSERT_VALUE_STRING(bsTestParseExpr("1.5e10"), "{\"number\":15000000000}");
    ASSERT_VALUE_STRING(bsTestParseExpr("3e-5"), "{\"number\":0.00003}");
    ASSERT_VALUE_STRING(bsTestParseExpr("0xFF"), "{\"number\":255}");
    ASSERT_VALUE_STRING(bsTestParseExpr("0x"), "Syntax error\n0x\n ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("1e"), "Syntax error\n1e\n ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("1 + 1e999"), "Number out of range\n1 + 1e999\n    ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("'abc'"), "{\"string\":\"abc\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("\"abc\""), "{\"string\":\"abc\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("'a\\'b'"), "{\"string\":\"a'b\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("'a\\\\b'"), "{\"string\":\"a\\\\b\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("'a\\nb'"), "{\"string\":\"a\\nb\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("'\\r\\t\\b\\f'"), "{\"string\":\"\\r\\t\\b\\f\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("'\\u0041'"), "{\"string\":\"A\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("'\\d'"), "{\"string\":\"\\\\d\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("'\\u00'"), "{\"string\":\"\\\\u00\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("'a\\\\'"), "{\"string\":\"a\\\\\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("x"), "{\"variable\":\"x\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("null"), "{\"variable\":\"null\"}");
}


TEST(parser_expression_operators)
{
    ASSERT_VALUE_STRING(bsTestParseExpr("1 + 2"),
                        "{\"binary\":{\"left\":{\"number\":1},\"op\":\"+\",\"right\":{\"number\":2}}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("1 + 2 * 3"),
                        "{\"binary\":{\"left\":{\"number\":1},\"op\":\"+\",\"right\":"
                        "{\"binary\":{\"left\":{\"number\":2},\"op\":\"*\",\"right\":{\"number\":3}}}}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("1 * 2 + 3"),
                        "{\"binary\":{\"left\":{\"binary\":{\"left\":{\"number\":1},\"op\":\"*\","
                        "\"right\":{\"number\":2}}},\"op\":\"+\",\"right\":{\"number\":3}}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("!a"),
                        "{\"unary\":{\"expr\":{\"variable\":\"a\"},\"op\":\"!\"}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("-a"),
                        "{\"unary\":{\"expr\":{\"variable\":\"a\"},\"op\":\"-\"}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("~a"),
                        "{\"unary\":{\"expr\":{\"variable\":\"a\"},\"op\":\"~\"}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("(a)"), "{\"group\":{\"variable\":\"a\"}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("a # comment"), "{\"variable\":\"a\"}");

    /* All the binary operators parse */
    static const char *operators[] = {"**", "*", "/", "%", "+", "-", "<<", ">>", "<=", "<", ">=", ">",
                                      "==", "!=", "&", "^", "|", "&&", "||"};
    for (size_t ix = 0; ix < sizeof(operators) / sizeof(operators[0]); ix++) {
        char text[32];
        char expected[128];
        snprintf(text, sizeof(text), "a %s b", operators[ix]);
        snprintf(expected, sizeof(expected),
                 "{\"binary\":{\"left\":{\"variable\":\"a\"},\"op\":\"%s\",\"right\":{\"variable\":\"b\"}}}",
                 operators[ix]);
        ASSERT_VALUE_STRING(bsTestParseExpr(text), expected);
    }
}


TEST(parser_expression_functions)
{
    ASSERT_VALUE_STRING(bsTestParseExpr("f()"), "{\"function\":{\"args\":[],\"name\":\"f\"}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("f(1)"),
                        "{\"function\":{\"args\":[{\"number\":1}],\"name\":\"f\"}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("f(1, 2)"),
                        "{\"function\":{\"args\":[{\"number\":1},{\"number\":2}],\"name\":\"f\"}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("f (1)"),
                        "{\"function\":{\"args\":[{\"number\":1}],\"name\":\"f\"}}");

    /* Many arguments grow the argument array */
    ASSERT_VALUE_STRING(bsTestParseExpr("f(1,2,3,4,5,6)"),
                        "{\"function\":{\"args\":[{\"number\":1},{\"number\":2},{\"number\":3},"
                        "{\"number\":4},{\"number\":5},{\"number\":6}],\"name\":\"f\"}}");
}


TEST(parser_expression_literals_compound)
{
    ASSERT_VALUE_STRING(bsTestParseExpr("{}"), "{\"function\":{\"args\":[],\"name\":\"objectNew\"}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("{'a': 1}"),
                        "{\"function\":{\"args\":[{\"string\":\"a\"},{\"number\":1}],\"name\":\"objectNew\"}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("{'a': 1, 'b': 2}"),
                        "{\"function\":{\"args\":[{\"string\":\"a\"},{\"number\":1},{\"string\":\"b\"},"
                        "{\"number\":2}],\"name\":\"objectNew\"}}");
    ASSERT_VALUE_STRING(bsTestParseExprArray("[]"), "{\"function\":{\"args\":[],\"name\":\"arrayNew\"}}");
    ASSERT_VALUE_STRING(bsTestParseExprArray("[1, 2]"),
                        "{\"function\":{\"args\":[{\"number\":1},{\"number\":2}],\"name\":\"arrayNew\"}}");

    /* Without array literals, brackets are a variable name */
    ASSERT_VALUE_STRING(bsTestParseExpr("[Height (ft)]"), "{\"variable\":\"Height (ft)\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("[ a ]"), "{\"variable\":\"a \"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("[a\\]b]"), "{\"variable\":\"a]b\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("[a\\\\b]"), "{\"variable\":\"a\\\\b\"}");
}


TEST(parser_expression_errors)
{
    ASSERT_VALUE_STRING(bsTestParseExpr(""), "Syntax error\n\n^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("1 +"), "Syntax error\n1 +\n   ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("(1"), "Unmatched parenthesis\n(1\n^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("f(1"), "Syntax error\nf(1\n   ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("f(1 2)"), "Syntax error\nf(1 2)\n   ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("'abc"), "Syntax error\n'abc\n^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("{'a'}"), "Syntax error\n{'a'}\n    ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("{'a': 1 'b': 2}"), "Syntax error\n{'a': 1 'b': 2}\n       ^\n");
    ASSERT_VALUE_STRING(bsTestParseExprArray("[1 2]"), "Syntax error\n[1 2]\n  ^\n");
    ASSERT_VALUE_STRING(bsTestParseExprArray("[1"), "Syntax error\n[1\n  ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("[]"), "Syntax error\n[]\n^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("[a"), "Syntax error\n[a\n^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("1 2"), "Syntax error\n1 2\n ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("!"), "Syntax error\n!\n ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("{1: "), "Syntax error\n{1: \n   ^\n");
    ASSERT_VALUE_STRING(bsTestParseExprArray("[1, "), "Syntax error\n[1, \n   ^\n");

    /* The error argument is optional */
    ASSERT_NULL(bsParseExpression("1 +", 3, 0, NULL, false, NULL));
    BSExpr *expr = bsParseExpression("1", 1, 0, NULL, false, NULL);
    ASSERT_NOT_NULL(expr);
    bsExprFree(expr);
    bsExprFree(NULL);
}


TEST(parser_expression_error_line_trim)
{
    /* A long error line is trimmed around the error column */
    BSValue text = bsTestRepeat(NULL, "1 + ", 100, "+");
    bsTestParseExprContains(bsStringData(text), "... ");
    bsRelease(text);

    /* An error near the start trims only the end */
    text = bsTestRepeat("+", " abcd", 100, NULL);
    bsTestParseExprContains(bsStringData(text), " ...");
    bsRelease(text);

    /* An error in the middle trims both ends */
    BSValue left = bsTestRepeat(NULL, "abcd", 50, " + + ");
    text = bsTestRepeat(bsStringData(left), "abcd", 50, NULL);
    bsRelease(left);
    bsTestParseExprContains(bsStringData(text), "... ");
    bsRelease(text);
}


TEST(parser_statements)
{
    ASSERT_VALUE_STRING(bsTestParse("a = 1"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"expr\":{\"expr\":{\"number\":1},"
                        "\"lineNumber\":1,\"name\":\"a\"}}]}");
    ASSERT_VALUE_STRING(bsTestParse("foo()"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"expr\":{\"expr\":"
                        "{\"function\":{\"args\":[],\"name\":\"foo\"}},\"lineNumber\":1}}]}");
    ASSERT_VALUE_STRING(bsTestParse("label:"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"label\":{\"lineNumber\":1,"
                        "\"name\":\"label\"}}]}");
    ASSERT_VALUE_STRING(bsTestParse("return"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"return\":{\"lineNumber\":1}}]}");
    ASSERT_VALUE_STRING(bsTestParse("return 1"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"return\":{\"expr\":{\"number\":1},"
                        "\"lineNumber\":1}}]}");
    ASSERT_VALUE_STRING(bsTestParse("return # nothing"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"return\":{\"lineNumber\":1}}]}");
    ASSERT_VALUE_STRING(bsTestParse("jump label"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"jump\":{\"label\":\"label\","
                        "\"lineNumber\":1}}]}");
    ASSERT_VALUE_STRING(bsTestParse("jumpif (a) label"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"jump\":{\"expr\":"
                        "{\"variable\":\"a\"},\"label\":\"label\",\"lineNumber\":1}}]}");

    /* Comments and blank lines are skipped */
    ASSERT_VALUE_STRING(bsTestParse("# comment\n\n   \na = 1  # trailing"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"expr\":{\"expr\":{\"number\":1},"
                        "\"lineNumber\":4,\"name\":\"a\"}}]}");
}


TEST(parser_includes)
{
    ASSERT_VALUE_STRING(bsTestParse("include 'a.bare'"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"include\":{\"includes\":"
                        "[{\"url\":\"a.bare\"}],\"lineNumber\":1}}]}");
    ASSERT_VALUE_STRING(bsTestParse("include <a.bare>"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"include\":{\"includes\":"
                        "[{\"system\":true,\"url\":\"a.bare\"}],\"lineNumber\":1}}]}");
    ASSERT_VALUE_STRING(bsTestParse("include 'a.bare'\ninclude 'b.bare'"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"include\":{\"includes\":"
                        "[{\"url\":\"a.bare\"},{\"url\":\"b.bare\"}],\"lineCount\":2,\"lineNumber\":1}}]}");
    ASSERT_VALUE_STRING(bsTestParse("include 'a\\'b.bare'"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"include\":{\"includes\":"
                        "[{\"url\":\"a'b.bare\"}],\"lineNumber\":1}}]}");

    /* An unterminated include is an expression statement, which fails to parse */
    bsTestParseContains("include 'a.bare", "Syntax error");
}


TEST(parser_continuation)
{
    ASSERT_VALUE_STRING(bsTestParse("a = 1 + \\\n    2"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"expr\":{\"expr\":{\"binary\":"
                        "{\"left\":{\"number\":1},\"op\":\"+\",\"right\":{\"number\":2}}},\"lineCount\":2,"
                        "\"lineNumber\":1,\"name\":\"a\"}}]}");
    ASSERT_VALUE_STRING(bsTestParse("a = 1 + \\\n    2 + \\\n    3"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"expr\":{\"expr\":{\"binary\":"
                        "{\"left\":{\"binary\":{\"left\":{\"number\":1},\"op\":\"+\",\"right\":{\"number\":2}}},"
                        "\"op\":\"+\",\"right\":{\"number\":3}}},\"lineCount\":3,\"lineNumber\":1,"
                        "\"name\":\"a\"}}]}");

    /* A trailing backslash with trailing whitespace also continues */
    ASSERT_VALUE_STRING(bsTestParse("a = 1 + \\  \n    2"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"expr\":{\"expr\":{\"binary\":"
                        "{\"left\":{\"number\":1},\"op\":\"+\",\"right\":{\"number\":2}}},\"lineCount\":2,"
                        "\"lineNumber\":1,\"name\":\"a\"}}]}");
}


TEST(parser_function)
{
    ASSERT_VALUE_STRING(bsTestParse("function f():\nendfunction"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"function\":{\"lineNumber\":1,"
                        "\"name\":\"f\",\"statements\":[]}}]}");
    ASSERT_VALUE_STRING(bsTestParse("function f(a, b):\n    return a\nendfunction"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"function\":{\"args\":[\"a\",\"b\"],"
                        "\"lineNumber\":1,\"name\":\"f\",\"statements\":[{\"return\":{\"expr\":"
                        "{\"variable\":\"a\"},\"lineNumber\":2}}]}}]}");
    ASSERT_VALUE_STRING(bsTestParse("async function f():\nendfunction"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"function\":{\"async\":true,"
                        "\"lineNumber\":1,\"name\":\"f\",\"statements\":[]}}]}");
    ASSERT_VALUE_STRING(bsTestParse("function f(a...):\nendfunction"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"function\":{\"args\":[\"a\"],"
                        "\"lastArgArray\":true,\"lineNumber\":1,\"name\":\"f\",\"statements\":[]}}]}");
    ASSERT_VALUE_STRING(bsTestParse("function f(...):\nendfunction"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"function\":{\"lastArgArray\":true,"
                        "\"lineNumber\":1,\"name\":\"f\",\"statements\":[]}}]}");

    /* Many arguments grow the argument array */
    ASSERT_VALUE_STRING(bsTestParse("function f(a,b,c,d,e,g,h,i,j):\nendfunction"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"function\":{\"args\":"
                        "[\"a\",\"b\",\"c\",\"d\",\"e\",\"g\",\"h\",\"i\",\"j\"],\"lineNumber\":1,"
                        "\"name\":\"f\",\"statements\":[]}}]}");
}


TEST(parser_function_errors)
{
    ASSERT_VALUE_STRING(bsTestParse("function f():\nfunction g():\nendfunction\nendfunction"),
                        "test.bare:2: Nested function definition\nfunction g():\n^\n");
    ASSERT_VALUE_STRING(bsTestParse("endfunction"),
                        "test.bare:1: No matching function definition\nendfunction\n^\n");
    ASSERT_VALUE_STRING(bsTestParse("function f():\n    if a:\nendfunction"),
                        "test.bare:2: Missing endif statement\n    if a:\n^\n");
    ASSERT_VALUE_STRING(bsTestParse("function f():\n    while a:\nendfunction"),
                        "test.bare:2: Missing endwhile statement\n    while a:\n^\n");
    ASSERT_VALUE_STRING(bsTestParse("function f():\n    for a in b:\nendfunction"),
                        "test.bare:2: Missing endfor statement\n    for a in b:\n^\n");

    /* A malformed function statement falls through to the expression parser */
    bsTestParseContains("function f(:", "Syntax error");
    bsTestParseContains("function ():", "Syntax error");
    bsTestParseContains("function f()", "Syntax error");
    bsTestParseContains("function f(a,):", "Syntax error");
    bsTestParseContains("functionf():", "Syntax error");
    bsTestParseContains("async f():", "Syntax error");
    bsTestParseContains("endfunction x", "Syntax error");
}


TEST(parser_structured_errors)
{
    ASSERT_VALUE_STRING(bsTestParse("endif"), "test.bare:1: No matching if statement\nendif\n^\n");
    ASSERT_VALUE_STRING(bsTestParse("else:"), "test.bare:1: No matching if statement\nelse:\n^\n");
    ASSERT_VALUE_STRING(bsTestParse("elif a:"), "test.bare:1: No matching if statement\nelif a:\n^\n");
    ASSERT_VALUE_STRING(bsTestParse("endwhile"), "test.bare:1: No matching while statement\nendwhile\n^\n");
    ASSERT_VALUE_STRING(bsTestParse("endfor"), "test.bare:1: No matching for statement\nendfor\n^\n");
    ASSERT_VALUE_STRING(bsTestParse("break"), "test.bare:1: Break statement outside of loop\nbreak\n^\n");
    ASSERT_VALUE_STRING(bsTestParse("continue"),
                        "test.bare:1: Continue statement outside of loop\ncontinue\n^\n");
    ASSERT_VALUE_STRING(bsTestParse("if a:\n    break\nendif"),
                        "test.bare:2: Break statement outside of loop\n    break\n^\n");
    ASSERT_VALUE_STRING(bsTestParse("if a:\nelse:\nelse:\nendif"),
                        "test.bare:3: Multiple else statements\nelse:\n^\n");
    ASSERT_VALUE_STRING(bsTestParse("if a:\nelse:\nelif b:\nendif"),
                        "test.bare:3: Elif statement following else statement\nelif b:\n^\n");
    ASSERT_VALUE_STRING(bsTestParse("if a:"), "test.bare:1: Missing endif statement\nif a:\n^\n");
    ASSERT_VALUE_STRING(bsTestParse("while a:"), "test.bare:1: Missing endwhile statement\nwhile a:\n^\n");
    ASSERT_VALUE_STRING(bsTestParse("for a in b:"), "test.bare:1: Missing endfor statement\nfor a in b:\n^\n");
    ASSERT_VALUE_STRING(bsTestParse("while a:\n    endif\nendwhile"),
                        "test.bare:2: No matching if statement\n    endif\n^\n");
    ASSERT_VALUE_STRING(bsTestParse("if a:\n    endwhile\nendif"),
                        "test.bare:2: No matching while statement\n    endwhile\n^\n");
    ASSERT_VALUE_STRING(bsTestParse("if a:\n    endfor\nendif"),
                        "test.bare:2: No matching for statement\n    endfor\n^\n");

    /* Expression errors inside structured statement headers */
    bsTestParseContains("if 1 +:\nendif", "Syntax error");
    bsTestParseContains("if a:\nelif 1 +:\nendif", "Syntax error");
    bsTestParseContains("while 1 +:\nendwhile", "Syntax error");
    bsTestParseContains("for a in 1 +:\nendfor", "Syntax error");
    bsTestParseContains("jumpif (1 +) label", "Syntax error");
    bsTestParseContains("return 1 +", "Syntax error");
    bsTestParseContains("a = 1 +", "Syntax error");

    /* Malformed structured statements fall through to the expression parser */
    bsTestParseContains("if a", "Syntax error");
    bsTestParseContains("for in b:\nendfor", "Syntax error");
    bsTestParseContains("for a b:\nendfor", "Syntax error");
    bsTestParseContains("for a, in b:\nendfor", "Syntax error");
    bsTestParseContains("jumpif x label", "Syntax error");
    bsTestParseContains("jump 1", "Syntax error");
    bsTestParseContains("jumpif (a) 1", "Syntax error");
    bsTestParseContains("break x", "Syntax error");
}


TEST(parser_no_script_name)
{
    BSParserError error = {0};
    BSScript *script = bsParseScript("a = 1", 5, 1, NULL, &error);
    ASSERT_NOT_NULL(script);
    ASSERT_INT_EQ(bsValueType(script->scriptName), BS_NULL);
    BSValue model = bsScriptToModel(script);
    ASSERT_FALSE(bsObjectHas(model, "scriptName"));
    bsRelease(model);
    bsScriptRelease(script);

    /* A parse error without a script name */
    script = bsParseScript("a = 1 +", 7, 1, NULL, &error);
    ASSERT_NULL(script);
    ASSERT_VALUE_STRING_KEEP(error.message, ":1: Syntax error\na = 1 +\n       ^\n");
    bsParserErrorFree(&error);

    /* A parse error with no error output */
    ASSERT_NULL(bsTestScript("a = 1 +", NULL));

    /* Script reference counting */
    script = bsTestScript("a = 1", NULL);
    bsScriptRetain(script);
    bsScriptRelease(script);
    bsScriptRelease(script);
    bsScriptRelease(NULL);
}


TEST(parser_coverage_gaps)
{
    /* A keyword prefix followed by an identifier character is not a keyword */
    bsTestParseContains("for a inx b:\nendfor", "Syntax error");
    bsTestParseContains("asyncx function f():\nendfunction", "Syntax error");

    /* Upper-case and invalid unicode string escapes */
    ASSERT_VALUE_STRING(bsTestParseExpr("'\\u00FF'"), "{\"string\":\"\xc3\xbf\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("'\\u00zz'"), "{\"string\":\"\\\\u00zz\"}");

    /* An object literal with a failing value expression */
    bsTestParseExprContains("{'a': 1 +}", "Syntax error");
    bsTestParseExprContains("{1 +: 2}", "Syntax error");

    /* An array literal with many values grows the argument array */
    ASSERT_VALUE_STRING(bsTestParseExprArray("[1,2,3,4,5]"),
                        "{\"function\":{\"args\":[{\"number\":1},{\"number\":2},{\"number\":3},"
                        "{\"number\":4},{\"number\":5}],\"name\":\"arrayNew\"}}");

    /* An object literal with many key/value pairs */
    ASSERT_VALUE_STRING(bsTestParseExpr("{'a':1,'b':2,'c':3}"),
                        "{\"function\":{\"args\":[{\"string\":\"a\"},{\"number\":1},{\"string\":\"b\"},"
                        "{\"number\":2},{\"string\":\"c\"},{\"number\":3}],\"name\":\"objectNew\"}}");

    /* A negative exponent in a number literal */
    ASSERT_VALUE_STRING(bsTestParseExpr("1e+3"), "{\"number\":1000}");
    ASSERT_VALUE_STRING(bsTestParseExpr("-1.5e-2"), "{\"number\":-0.015}");

    /* A group with a failing inner expression */
    bsTestParseExprContains("(1 +)", "Syntax error");

    /* A binary expression with a failing right operand */
    bsTestParseExprContains("1 + *", "Syntax error");

    /* An assignment whose expression is only whitespace */
    ASSERT_VALUE_STRING(bsTestParse("a = "), "test.bare:1: Syntax error\na = \n   ^\n");

    /* Windows line endings */
    ASSERT_VALUE_STRING(bsTestParse("a = 1\r\nreturn a"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"expr\":{\"expr\":{\"number\":1},"
                        "\"lineNumber\":1,\"name\":\"a\"}},{\"return\":{\"expr\":{\"variable\":\"a\"},"
                        "\"lineNumber\":2}}]}");

    /* A continuation whose continued line has trailing whitespace */
    ASSERT_VALUE_STRING(bsTestParse("a = 1 + \\\n    2   \\\n    + 3"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"expr\":{\"expr\":{\"binary\":"
                        "{\"left\":{\"binary\":{\"left\":{\"number\":1},\"op\":\"+\",\"right\":{\"number\":2}}},"
                        "\"op\":\"+\",\"right\":{\"number\":3}}},\"lineCount\":3,\"lineNumber\":1,"
                        "\"name\":\"a\"}}]}");

    /* A function definition with a malformed argument list frees its parsed arguments */
    bsTestParseContains("function f(a, b:", "Syntax error");
    /* A keyword followed by a colon is a label definition, as in the reference parser */
    ASSERT_VALUE_STRING(bsTestParse("function :"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"label\":{\"lineNumber\":1,"
                        "\"name\":\"function\"}}]}");
}


TEST(parser_while_expression_copy)
{
    /* The while-do footer jump deep-copies the loop test expression of every expression kind */
    ASSERT_VALUE(bsTestExecute("i = 0\nwhile 'x':\n    i = i + 1\n    if i > 2:\n        break\n"
                               "    endif\nendwhile\nreturn i"), "3");
    ASSERT_VALUE(bsTestExecute("i = 0\nfunction test():\n    return i < 3\nendfunction\n"
                               "while test():\n    i = i + 1\nendwhile\nreturn i"), "3");
    ASSERT_VALUE(bsTestExecute("i = 5\nwhile !(i == 0):\n    i = i - 1\nendwhile\nreturn i"), "0");
    ASSERT_VALUE(bsTestExecute("i = 0\nwhile (i < 3):\n    i = i + 1\nendwhile\nreturn i"), "3");
    ASSERT_VALUE(bsTestExecute("i = 0\nwhile [1]:\n    i = i + 1\n    if i > 2:\n        break\n"
                               "    endif\nendwhile\nreturn i"), "3");
}


TEST(parser_group_slot_resolution)
{
    /* A group expression inside a function resolves its variable slots */
    ASSERT_VALUE(bsTestExecute("function f():\n    a = 2\n    return (a + 1) * 2\nendfunction\nreturn f()"),
                 "6");
    ASSERT_VALUE(bsTestExecute("function f(a):\n    return -a\nendfunction\nreturn f(3)"), "-3");
    ASSERT_VALUE(bsTestExecute("function f(a):\n    jumpif (a) yes\n    return 'no'\n    yes:\n"
                               "    return 'yes'\nendfunction\nreturn f(true)"), "\"yes\"");
}


TEST(parser_statement_variants)
{
    /* An else-then without an elif */
    ASSERT_VALUE(bsTestExecute("if false:\n    return 1\nelse:\n    return 2\nendif"), "2");

    /* Nested loops and structured statements inside a function */
    ASSERT_VALUE(bsTestExecute("function f():\n    total = 0\n    for a in [1, 2]:\n"
                               "        for b in [10, 20]:\n            if b == 20:\n"
                               "                continue\n            endif\n"
                               "            total = total + a * b\n        endfor\n    endfor\n"
                               "    return total\nendfunction\nreturn f()"), "30");
    ASSERT_VALUE(bsTestExecute("function f():\n    i = 0\n    while true:\n        i = i + 1\n"
                               "        if i > 2:\n            break\n        endif\n    endwhile\n"
                               "    return i\nendfunction\nreturn f()"), "3");

    /* A label statement inside a function */
    ASSERT_VALUE(bsTestExecute("function f():\n    top:\n    return 1\nendfunction\nreturn f()"), "1");
}


TEST(parser_keyword_fallthrough)
{
    /*
     * A statement keyword that does not match its pattern falls through to the next check, and
     * ultimately to the expression parser - where a bare keyword is just a variable name
     */
    bsTestParseContains("if a:\nelif\nendif", "\"variable\":\"elif\"");
    bsTestParseContains("if a:\nelse\nendif", "\"variable\":\"else\"");
    bsTestParseContains("while x\nendwhile", "Syntax error");
    bsTestParseContains("for x\nendfor", "Syntax error");
    bsTestParseContains("if a:\nelif x\nendif", "Syntax error");

    /* A keyword immediately followed by "(" has no whitespace for the header expression */
    bsTestParseContains("if(a):\nendif", "Syntax error");
    bsTestParseContains("while(a):\nendwhile", "Syntax error");
    bsTestParseContains("if a:\nelif(b):\nendif", "Syntax error");
    bsTestParseContains("for a in(b):\nendfor", "Syntax error");

    /* "return(...)" has no whitespace, so it parses as a call to a function named "return" */
    ASSERT_VALUE_STRING(bsTestParse("return(1)"),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"expr\":{\"expr\":{\"function\":"
                        "{\"args\":[{\"number\":1}],\"name\":\"return\"}},\"lineNumber\":1}}]}");

    /* An assignment with nothing after the equals sign is not an assignment */
    ASSERT_VALUE_STRING(bsTestParse("a ="), "test.bare:1: Syntax error\na =\n ^\n");

    /* A trailing continuation line with trailing whitespace */
    ASSERT_VALUE_STRING(bsTestParse("a = 1 + \\\n    2   "),
                        "{\"scriptName\":\"test.bare\",\"statements\":[{\"expr\":{\"expr\":{\"binary\":"
                        "{\"left\":{\"number\":1},\"op\":\"+\",\"right\":{\"number\":2}}},\"lineCount\":2,"
                        "\"lineNumber\":1,\"name\":\"a\"}}]}");
}


TEST(parser_final_coverage)
{
    /* A function call whose first argument fails to parse */
    ASSERT_VALUE_STRING(bsTestParseExpr("f("), "Syntax error\nf(\n  ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("f(1,)"), "Syntax error\nf(1,)\n    ^\n");

    /* A "0x" prefix with no hex digits parses as the number zero followed by an identifier */
    ASSERT_VALUE_STRING(bsTestParseExpr("0xzz"), "Syntax error\n0xzz\n ^\n");

    /* A function definition whose name is not followed by an open parenthesis */
    bsTestParseContains("function f:", "Syntax error");

    /* A nested function definition with arguments frees its parsed argument names */
    ASSERT_VALUE_STRING(bsTestParse("function f():\n    function g(a, b):\n    endfunction\nendfunction"),
                        "test.bare:2: Nested function definition\n    function g(a, b):\n^\n");
}


TEST(parser_bootstrap_errors)
{
    /* A deeply nested expression exceeds the parser's own expression depth */
    BSValue open = bsTestRepeat(NULL, "(", 600, "1");
    BSValue text = bsTestRepeat(bsStringData(open), ")", 600, NULL);
    bsRelease(open);

    /* Reported against the script being parsed, not against the parser */
    BSParserError error = {0};
    ASSERT_NULL(bsParseExpression(bsStringData(text), bsStringSize(text), 0, "deep.bare", false, &error));
    ASSERT_VALUE_STRING_KEEP(error.message, "deep.bare: Maximum expression depth exceeded\n");
    ASSERT_VALUE_STRING_KEEP(error.error, "Maximum expression depth exceeded");
    bsParserErrorFree(&error);

    /* Without a script name the message has no location */
    ASSERT_NULL(bsParseExpression(bsStringData(text), bsStringSize(text), 0, NULL, false, &error));
    ASSERT_VALUE_STRING_KEEP(error.message, "Maximum expression depth exceeded\n");
    ASSERT_INT_EQ(bsValueType(error.scriptName), BS_NULL);
    bsParserErrorFree(&error);

    /* The error argument is optional */
    ASSERT_NULL(bsParseExpression(bsStringData(text), bsStringSize(text), 0, NULL, false, NULL));
    ASSERT_NULL(bsTestScript(bsStringData(text), NULL));
    bsRelease(text);
}


TEST(parser_lint)
{
    static const char *text = "function f():\n    unused = 1\n    return 2\nendfunction\n1 + 2\n";
    BSScript *script = bsTestScript(text, "lint.bare");
    ASSERT_NOT_NULL(script);

    BSValue warnings = bsLintScript(script, bsNull());
    ASSERT_TRUE(bsArrayCount(warnings) >= 2);
    BSValue json = bsJSONEncode(warnings, 0);
    ASSERT_STR_CONTAINS(bsStringData(json), "Unused variable \\\"unused\\\"");
    ASSERT_STR_CONTAINS(bsStringData(json), "Pointless global statement");
    bsRelease(json);
    bsRelease(warnings);

    /* Linting with the script's globals resolves function references */
    BSOptions *options = bsTestOptions();
    bsRelease(bsExecuteScript(script, options));
    warnings = bsLintScript(script, options->globals);
    ASSERT_TRUE(bsArrayCount(warnings) >= 1);
    bsRelease(warnings);
    bsOptionsFree(options);
    bsScriptRelease(script);

    /* A clean script lints without warnings */
    static const char *clean = "function f(a):\n    return a\nendfunction\nreturn f(1)\n";
    script = bsTestScript(clean, "clean.bare");
    warnings = bsLintScript(script, bsNull());
    ASSERT_VALUE(warnings, "[]");
    bsScriptRelease(script);
}
