/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The parser unit tests
 */

#include <stdio.h>
#include <string.h>

#include "test.h"


/* Parse script text and return its model as JSON, or the parse error message */
static BSValue bsTestParse(const char *text)
{
    BSParserError error;
    memset(&error, 0, sizeof(error));
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


/* Parse expression text and return its model as JSON, or the parse error message */
static BSValue bsTestParseExpr(const char *text, bool arrayLiterals)
{
    BSParserError error;
    memset(&error, 0, sizeof(error));
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


TEST(parser_expression_literals)
{
    ASSERT_VALUE_STRING(bsTestParseExpr("5", false), "{\"number\":5}");
    ASSERT_VALUE_STRING(bsTestParseExpr("-5", false), "{\"number\":-5}");
    ASSERT_VALUE_STRING(bsTestParseExpr("+5", false), "{\"number\":5}");
    ASSERT_VALUE_STRING(bsTestParseExpr("3.14", false), "{\"number\":3.14}");
    ASSERT_VALUE_STRING(bsTestParseExpr("5.", false), "{\"number\":5}");
    ASSERT_VALUE_STRING(bsTestParseExpr("1.5e10", false), "{\"number\":15000000000}");
    ASSERT_VALUE_STRING(bsTestParseExpr("3e-5", false), "{\"number\":0.00003}");
    ASSERT_VALUE_STRING(bsTestParseExpr("0xFF", false), "{\"number\":255}");
    ASSERT_VALUE_STRING(bsTestParseExpr("0x", false), "Syntax error\n0x\n ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("1e", false), "Syntax error\n1e\n ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("'abc'", false), "{\"string\":\"abc\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("\"abc\"", false), "{\"string\":\"abc\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("'a\\'b'", false), "{\"string\":\"a'b\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("'a\\\\b'", false), "{\"string\":\"a\\\\b\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("'a\\nb'", false), "{\"string\":\"a\\nb\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("'\\r\\t\\b\\f'", false), "{\"string\":\"\\r\\t\\b\\f\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("'\\u0041'", false), "{\"string\":\"A\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("'\\d'", false), "{\"string\":\"\\\\d\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("'\\u00'", false), "{\"string\":\"\\\\u00\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("'a\\\\'", false), "{\"string\":\"a\\\\\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("x", false), "{\"variable\":\"x\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("null", false), "{\"variable\":\"null\"}");
}


TEST(parser_expression_operators)
{
    ASSERT_VALUE_STRING(bsTestParseExpr("1 + 2", false),
                        "{\"binary\":{\"left\":{\"number\":1},\"op\":\"+\",\"right\":{\"number\":2}}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("1 + 2 * 3", false),
                        "{\"binary\":{\"left\":{\"number\":1},\"op\":\"+\",\"right\":"
                        "{\"binary\":{\"left\":{\"number\":2},\"op\":\"*\",\"right\":{\"number\":3}}}}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("1 * 2 + 3", false),
                        "{\"binary\":{\"left\":{\"binary\":{\"left\":{\"number\":1},\"op\":\"*\","
                        "\"right\":{\"number\":2}}},\"op\":\"+\",\"right\":{\"number\":3}}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("!a", false),
                        "{\"unary\":{\"expr\":{\"variable\":\"a\"},\"op\":\"!\"}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("-a", false),
                        "{\"unary\":{\"expr\":{\"variable\":\"a\"},\"op\":\"-\"}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("~a", false),
                        "{\"unary\":{\"expr\":{\"variable\":\"a\"},\"op\":\"~\"}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("(a)", false), "{\"group\":{\"variable\":\"a\"}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("a # comment", false), "{\"variable\":\"a\"}");

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
        ASSERT_VALUE_STRING(bsTestParseExpr(text, false), expected);
    }
}


TEST(parser_expression_functions)
{
    ASSERT_VALUE_STRING(bsTestParseExpr("f()", false), "{\"function\":{\"args\":[],\"name\":\"f\"}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("f(1)", false),
                        "{\"function\":{\"args\":[{\"number\":1}],\"name\":\"f\"}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("f(1, 2)", false),
                        "{\"function\":{\"args\":[{\"number\":1},{\"number\":2}],\"name\":\"f\"}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("f (1)", false),
                        "{\"function\":{\"args\":[{\"number\":1}],\"name\":\"f\"}}");

    /* Many arguments grow the argument array */
    ASSERT_VALUE_STRING(bsTestParseExpr("f(1,2,3,4,5,6)", false),
                        "{\"function\":{\"args\":[{\"number\":1},{\"number\":2},{\"number\":3},"
                        "{\"number\":4},{\"number\":5},{\"number\":6}],\"name\":\"f\"}}");
}


TEST(parser_expression_literals_compound)
{
    ASSERT_VALUE_STRING(bsTestParseExpr("{}", false), "{\"function\":{\"args\":[],\"name\":\"objectNew\"}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("{'a': 1}", false),
                        "{\"function\":{\"args\":[{\"string\":\"a\"},{\"number\":1}],\"name\":\"objectNew\"}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("{'a': 1, 'b': 2}", false),
                        "{\"function\":{\"args\":[{\"string\":\"a\"},{\"number\":1},{\"string\":\"b\"},"
                        "{\"number\":2}],\"name\":\"objectNew\"}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("[]", true), "{\"function\":{\"args\":[],\"name\":\"arrayNew\"}}");
    ASSERT_VALUE_STRING(bsTestParseExpr("[1, 2]", true),
                        "{\"function\":{\"args\":[{\"number\":1},{\"number\":2}],\"name\":\"arrayNew\"}}");

    /* Without array literals, brackets are a variable name */
    ASSERT_VALUE_STRING(bsTestParseExpr("[Height (ft)]", false), "{\"variable\":\"Height (ft)\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("[ a ]", false), "{\"variable\":\"a \"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("[a\\]b]", false), "{\"variable\":\"a]b\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("[a\\\\b]", false), "{\"variable\":\"a\\\\b\"}");
}


TEST(parser_expression_errors)
{
    ASSERT_VALUE_STRING(bsTestParseExpr("", false), "Syntax error\n\n^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("1 +", false), "Syntax error\n1 +\n   ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("(1", false), "Unmatched parenthesis\n(1\n^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("f(1", false), "Syntax error\nf(1\n   ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("f(1 2)", false), "Syntax error\nf(1 2)\n   ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("'abc", false), "Syntax error\n'abc\n^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("{'a'}", false), "Syntax error\n{'a'}\n    ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("{'a': 1 'b': 2}", false), "Syntax error\n{'a': 1 'b': 2}\n       ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("[1 2]", true), "Syntax error\n[1 2]\n  ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("[1", true), "Syntax error\n[1\n  ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("[]", false), "Syntax error\n[]\n^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("[a", false), "Syntax error\n[a\n^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("1 2", false), "Syntax error\n1 2\n ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("!", false), "Syntax error\n!\n ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("{1: ", false), "Syntax error\n{1: \n   ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("[1, ", true), "Syntax error\n[1, \n   ^\n");

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
    BSStringBuilder sb;
    bsSBInit(&sb);
    for (int ix = 0; ix < 100; ix++) {
        bsSBAppendString(&sb, "1 + ");
    }
    bsSBAppendChar(&sb, '+');
    BSValue text = bsSBToValue(&sb);
    BSValue message = bsTestParseExpr(bsStringData(text), false);
    ASSERT_TRUE(strstr(bsStringData(message), "... ") != NULL);
    bsRelease(message);
    bsRelease(text);

    /* An error near the start trims only the end */
    bsSBInit(&sb);
    bsSBAppendChar(&sb, '+');
    for (int ix = 0; ix < 100; ix++) {
        bsSBAppendString(&sb, " abcd");
    }
    text = bsSBToValue(&sb);
    message = bsTestParseExpr(bsStringData(text), false);
    ASSERT_TRUE(strstr(bsStringData(message), " ...") != NULL);
    bsRelease(message);
    bsRelease(text);

    /* An error in the middle trims both ends */
    bsSBInit(&sb);
    for (int ix = 0; ix < 50; ix++) {
        bsSBAppendString(&sb, "abcd");
    }
    bsSBAppendString(&sb, " + + ");
    for (int ix = 0; ix < 50; ix++) {
        bsSBAppendString(&sb, "abcd");
    }
    text = bsSBToValue(&sb);
    message = bsTestParseExpr(bsStringData(text), false);
    ASSERT_TRUE(strstr(bsStringData(message), "... ") != NULL);
    bsRelease(message);
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
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("include 'a.bare")), "Syntax error") != NULL);
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
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("function f(:")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("function ():")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("function f()")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("function f(a,):")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("functionf():")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("async f():")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("endfunction x")), "Syntax error") != NULL);
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
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("if 1 +:\nendif")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("if a:\nelif 1 +:\nendif")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("while 1 +:\nendwhile")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("for a in 1 +:\nendfor")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("jumpif (1 +) label")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("return 1 +")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("a = 1 +")), "Syntax error") != NULL);

    /* Malformed structured statements fall through to the expression parser */
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("if a")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("for in b:\nendfor")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("for a b:\nendfor")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("for a, in b:\nendfor")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("jumpif x label")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("jump 1")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("jumpif (a) 1")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("break x")), "Syntax error") != NULL);
}


TEST(parser_no_script_name)
{
    BSParserError error;
    memset(&error, 0, sizeof(error));
    BSScript *script = bsParseScript("a = 1", 5, 1, NULL, &error);
    ASSERT_NOT_NULL(script);
    ASSERT_INT_EQ(script->scriptName.type, BS_NULL);
    BSValue model = bsScriptToModel(script);
    ASSERT_FALSE(bsObjectHas(model, "scriptName"));
    bsRelease(model);
    bsScriptRelease(script);

    /* A parse error without a script name */
    script = bsParseScript("a = 1 +", 7, 1, NULL, &error);
    ASSERT_NULL(script);
    ASSERT_VALUE_STRING(bsRetain(error.message), ":1: Syntax error\na = 1 +\n       ^\n");
    bsParserErrorFree(&error);

    /* A parse error with no error output */
    ASSERT_NULL(bsParseScript("a = 1 +", 7, 1, NULL, NULL));

    /* Script reference counting */
    script = bsParseScript("a = 1", 5, 1, NULL, NULL);
    bsScriptRetain(script);
    bsScriptRelease(script);
    bsScriptRelease(script);
    bsScriptRelease(NULL);
}


TEST(parser_coverage_gaps)
{
    /* A keyword prefix followed by an identifier character is not a keyword */
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("for a inx b:\nendfor")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("asyncx function f():\nendfunction")), "Syntax error") != NULL);

    /* Upper-case and invalid unicode string escapes */
    ASSERT_VALUE_STRING(bsTestParseExpr("'\\u00FF'", false), "{\"string\":\"\xc3\xbf\"}");
    ASSERT_VALUE_STRING(bsTestParseExpr("'\\u00zz'", false), "{\"string\":\"\\\\u00zz\"}");

    /* An object literal with a failing value expression */
    ASSERT_TRUE(strstr(bsStringData(bsTestParseExpr("{'a': 1 +}", false)), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParseExpr("{1 +: 2}", false)), "Syntax error") != NULL);

    /* An array literal with many values grows the argument array */
    ASSERT_VALUE_STRING(bsTestParseExpr("[1,2,3,4,5]", true),
                        "{\"function\":{\"args\":[{\"number\":1},{\"number\":2},{\"number\":3},"
                        "{\"number\":4},{\"number\":5}],\"name\":\"arrayNew\"}}");

    /* An object literal with many key/value pairs */
    ASSERT_VALUE_STRING(bsTestParseExpr("{'a':1,'b':2,'c':3}", false),
                        "{\"function\":{\"args\":[{\"string\":\"a\"},{\"number\":1},{\"string\":\"b\"},"
                        "{\"number\":2},{\"string\":\"c\"},{\"number\":3}],\"name\":\"objectNew\"}}");

    /* A negative exponent in a number literal */
    ASSERT_VALUE_STRING(bsTestParseExpr("1e+3", false), "{\"number\":1000}");
    ASSERT_VALUE_STRING(bsTestParseExpr("-1.5e-2", false), "{\"number\":-0.015}");

    /* A group with a failing inner expression */
    ASSERT_TRUE(strstr(bsStringData(bsTestParseExpr("(1 +)", false)), "Syntax error") != NULL);

    /* A binary expression with a failing right operand */
    ASSERT_TRUE(strstr(bsStringData(bsTestParseExpr("1 + *", false)), "Syntax error") != NULL);

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
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("function f(a, b:")), "Syntax error") != NULL);
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
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("if a:\nelif\nendif")), "\"variable\":\"elif\"") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("if a:\nelse\nendif")), "\"variable\":\"else\"") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("while x\nendwhile")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("for x\nendfor")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("if a:\nelif x\nendif")), "Syntax error") != NULL);

    /* A keyword immediately followed by "(" has no whitespace for the header expression */
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("if(a):\nendif")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("while(a):\nendwhile")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("if a:\nelif(b):\nendif")), "Syntax error") != NULL);
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("for a in(b):\nendfor")), "Syntax error") != NULL);

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
    ASSERT_VALUE_STRING(bsTestParseExpr("f(", false), "Syntax error\nf(\n  ^\n");
    ASSERT_VALUE_STRING(bsTestParseExpr("f(1,)", false), "Syntax error\nf(1,)\n    ^\n");

    /* A "0x" prefix with no hex digits parses as the number zero followed by an identifier */
    ASSERT_VALUE_STRING(bsTestParseExpr("0xzz", false), "Syntax error\n0xzz\n ^\n");

    /* A function definition whose name is not followed by an open parenthesis */
    ASSERT_TRUE(strstr(bsStringData(bsTestParse("function f:")), "Syntax error") != NULL);

    /* A nested function definition with arguments frees its parsed argument names */
    ASSERT_VALUE_STRING(bsTestParse("function f():\n    function g(a, b):\n    endfunction\nendfunction"),
                        "test.bare:2: Nested function definition\n    function g(a, b):\n^\n");
}


TEST(parser_bootstrap_errors)
{
    /* A deeply nested expression exceeds the parser's own expression depth */
    BSStringBuilder sb;
    bsSBInit(&sb);
    for (int ix = 0; ix < 600; ix++) {
        bsSBAppendChar(&sb, '(');
    }
    bsSBAppendChar(&sb, '1');
    for (int ix = 0; ix < 600; ix++) {
        bsSBAppendChar(&sb, ')');
    }
    BSValue text = bsSBToValue(&sb);

    /* Reported against the script being parsed, not against the parser */
    BSParserError error;
    memset(&error, 0, sizeof(error));
    ASSERT_NULL(bsParseExpression(bsStringData(text), bsStringSize(text), 0, "deep.bare", false, &error));
    ASSERT_VALUE_STRING(bsRetain(error.message), "deep.bare: Maximum expression depth exceeded\n");
    ASSERT_VALUE_STRING(bsRetain(error.error), "Maximum expression depth exceeded");
    bsParserErrorFree(&error);

    /* Without a script name the message has no location */
    memset(&error, 0, sizeof(error));
    ASSERT_NULL(bsParseExpression(bsStringData(text), bsStringSize(text), 0, NULL, false, &error));
    ASSERT_VALUE_STRING(bsRetain(error.message), "Maximum expression depth exceeded\n");
    ASSERT_INT_EQ(error.scriptName.type, BS_NULL);
    bsParserErrorFree(&error);

    /* The error argument is optional */
    ASSERT_NULL(bsParseExpression(bsStringData(text), bsStringSize(text), 0, NULL, false, NULL));
    ASSERT_NULL(bsParseScript(bsStringData(text), bsStringSize(text), 1, NULL, NULL));
    bsRelease(text);
}


TEST(parser_lint)
{
    static const char *text = "function f():\n    unused = 1\n    return 2\nendfunction\n1 + 2\n";
    BSParserError error;
    memset(&error, 0, sizeof(error));
    BSScript *script = bsParseScript(text, strlen(text), 1, "lint.bare", &error);
    ASSERT_NOT_NULL(script);

    BSValue warnings = bsLintScript(script, bsNull());
    ASSERT_TRUE(bsArrayCount(warnings) >= 2);
    BSValue json = bsJSONEncode(warnings, 0);
    ASSERT_TRUE(strstr(bsStringData(json), "Unused variable \\\"unused\\\"") != NULL);
    ASSERT_TRUE(strstr(bsStringData(json), "Pointless global statement") != NULL);
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
    script = bsParseScript(clean, strlen(clean), 1, "clean.bare", &error);
    warnings = bsLintScript(script, bsNull());
    ASSERT_VALUE(warnings, "[]");
    bsScriptRelease(script);
}
