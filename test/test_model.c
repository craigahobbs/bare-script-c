/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The model conversion unit tests
 */

#include <stdio.h>
#include <string.h>

#include "test.h"


/* Decode a JSON model and convert it to a compiled script; NULL if the model is invalid */
static BSScript *bsTestScriptFromJSON(const char *json)
{
    BSValue model = bsJSONDecode(json, strlen(json), NULL);
    BSScript *script = bsScriptFromModel(model, NULL);
    bsRelease(model);
    return script;
}


/* Assert a model is rejected */
static void bsTestInvalidModel(const char *json)
{
    BSScript *script = bsTestScriptFromJSON(json);
    if (script != NULL) {
        bsScriptRelease(script);
        bsTestFail(__FILE__, __LINE__, "expected an invalid model: %s", json);
    } else {
        bsTestPass();
    }
}


TEST(model_script_round_trip)
{
    /* A parsed script converts to a model and back to an equivalent script */
    static const char *text =
        "include <unittest.bare>\n"
        "include 'local.bare'\n"
        "async function outer(a, b...):\n"
        "    c = a + b\n"
        "    if c:\n"
        "        return c\n"
        "    endif\n"
        "    top:\n"
        "    jumpif (c) top\n"
        "    jump top\n"
        "    return\n"
        "endfunction\n"
        "x = 1 + f(2, 'three') * -y || (z) && !w\n"
        "outer(1)\n";

    BSParserError error;
    memset(&error, 0, sizeof(error));
    BSScript *script = bsParseScript(text, strlen(text), 1, "round.bare", &error);
    ASSERT_NOT_NULL(script);

    BSValue model = bsScriptToModel(script);
    BSScript *script2 = bsScriptFromModel(model, NULL);
    ASSERT_NOT_NULL(script2);
    BSValue model2 = bsScriptToModel(script2);
    ASSERT_INT_EQ(bsValueCompare(model, model2), 0);

    /* The script name and the system flag survive the round trip */
    ASSERT_VALUE_STRING(bsRetain(script2->scriptName), "round.bare");
    script->system = true;
    BSValue systemModel = bsScriptToModel(script);
    BSScript *script3 = bsScriptFromModel(systemModel, NULL);
    ASSERT_TRUE(script3->system);

    bsRelease(model);
    bsRelease(model2);
    bsRelease(systemModel);
    bsScriptRelease(script);
    bsScriptRelease(script2);
    bsScriptRelease(script3);
}


TEST(model_script_name_override)
{
    BSScript *script = bsTestScriptFromJSON("{\"statements\":[],\"scriptName\":\"from-model.bare\"}");
    ASSERT_NOT_NULL(script);
    ASSERT_VALUE_STRING(bsRetain(script->scriptName), "from-model.bare");
    bsScriptRelease(script);

    BSValue model = bsJSONDecode("{\"statements\":[]}", 17, NULL);
    BSScript *named = bsScriptFromModel(model, "override.bare");
    ASSERT_VALUE_STRING(bsRetain(named->scriptName), "override.bare");
    bsScriptRelease(named);

    BSScript *unnamed = bsScriptFromModel(model, NULL);
    ASSERT_INT_EQ(unnamed->scriptName.type, BS_NULL);
    bsScriptRelease(unnamed);
    bsRelease(model);
}


TEST(model_script_lines)
{
    BSScript *script = bsTestScriptFromJSON("{\"statements\":[],\"scriptLines\":[\"a = 1\"]}");
    ASSERT_NOT_NULL(script);
    ASSERT_INT_EQ(bsArrayCount(script->scriptLines), 1);
    BSValue model = bsScriptToModel(script);
    ASSERT_TRUE(bsObjectHas(model, "scriptLines"));
    bsRelease(model);
    bsScriptRelease(script);
}


TEST(model_invalid)
{
    /* The script itself */
    bsTestInvalidModel("1");
    bsTestInvalidModel("{}");
    bsTestInvalidModel("{\"statements\":1}");

    /* A statement that is not an object, or has no recognized kind */
    bsTestInvalidModel("{\"statements\":[1]}");
    bsTestInvalidModel("{\"statements\":[{}]}");
    bsTestInvalidModel("{\"statements\":[{\"nope\":{}}]}");

    /* An expression statement */
    bsTestInvalidModel("{\"statements\":[{\"expr\":{\"expr\":1}}]}");

    /* A jump statement */
    bsTestInvalidModel("{\"statements\":[{\"jump\":{}}]}");
    bsTestInvalidModel("{\"statements\":[{\"jump\":{\"label\":1}}]}");
    bsTestInvalidModel("{\"statements\":[{\"jump\":{\"label\":\"a\",\"expr\":1}}]}");

    /* A return statement */
    bsTestInvalidModel("{\"statements\":[{\"return\":{\"expr\":1}}]}");

    /* A label statement */
    bsTestInvalidModel("{\"statements\":[{\"label\":{}}]}");
    bsTestInvalidModel("{\"statements\":[{\"label\":{\"name\":1}}]}");

    /* A function definition statement */
    bsTestInvalidModel("{\"statements\":[{\"function\":{}}]}");
    bsTestInvalidModel("{\"statements\":[{\"function\":{\"name\":\"f\"}}]}");
    bsTestInvalidModel("{\"statements\":[{\"function\":{\"name\":1,\"statements\":[]}}]}");
    bsTestInvalidModel("{\"statements\":[{\"function\":{\"name\":\"f\",\"args\":[1],\"statements\":[]}}]}");
    bsTestInvalidModel("{\"statements\":[{\"function\":{\"name\":\"f\",\"statements\":[{}]}}]}");

    /* An include statement */
    bsTestInvalidModel("{\"statements\":[{\"include\":{}}]}");
    bsTestInvalidModel("{\"statements\":[{\"include\":{\"includes\":[]}}]}");
    bsTestInvalidModel("{\"statements\":[{\"include\":{\"includes\":[{}]}}]}");
    bsTestInvalidModel("{\"statements\":[{\"include\":{\"includes\":[{\"url\":1}]}}]}");
}


TEST(model_valid_shapes)
{
    /* Each statement kind converts and executes */
    BSScript *script = bsTestScriptFromJSON(
        "{\"statements\":["
        "{\"expr\":{\"name\":\"a\",\"expr\":{\"number\":1},\"lineNumber\":1,\"lineCount\":2}},"
        "{\"label\":{\"name\":\"top\"}},"
        "{\"jump\":{\"label\":\"done\",\"expr\":{\"variable\":\"a\"}}},"
        "{\"jump\":{\"label\":\"top\"}},"
        "{\"label\":{\"name\":\"done\"}},"
        "{\"function\":{\"name\":\"f\",\"args\":[\"x\"],\"lastArgArray\":true,\"async\":true,"
        "\"statements\":[{\"return\":{\"expr\":{\"variable\":\"x\"}}}]}},"
        "{\"return\":{\"expr\":{\"function\":{\"name\":\"f\",\"args\":[{\"number\":2}]}}}}"
        "]}");
    ASSERT_NOT_NULL(script);
    ASSERT_INT_EQ(script->statements[0]->lineNumber, 1);
    ASSERT_INT_EQ(script->statements[0]->lineCount, 2);

    BSOptions *options = bsTestOptions();
    ASSERT_VALUE(bsExecuteScript(script, options), "[2]");
    bsOptionsFree(options);

    /* An include statement's system flag round-trips */
    BSValue model = bsScriptToModel(script);
    bsRelease(model);
    bsScriptRelease(script);

    script = bsTestScriptFromJSON(
        "{\"statements\":[{\"include\":{\"includes\":["
        "{\"url\":\"a.bare\"},{\"url\":\"b.bare\",\"system\":true}]}}]}");
    ASSERT_NOT_NULL(script);
    ASSERT_FALSE(script->statements[0]->u.include.includes[0].system);
    ASSERT_TRUE(script->statements[0]->u.include.includes[1].system);
    model = bsScriptToModel(script);
    BSValue includes = bsObjectGet(bsObjectGet(bsArrayGet(bsObjectGet(model, "statements"), 0),
                                               "include"), "includes");
    ASSERT_FALSE(bsObjectHas(bsArrayGet(includes, 0), "system"));
    ASSERT_TRUE(bsObjectHas(bsArrayGet(includes, 1), "system"));
    bsRelease(model);
    bsScriptRelease(script);
}


TEST(model_expression_shapes)
{
    /* Every expression kind converts to a model and back */
    static const char *expressions[] = {
        "1", "'a'", "x", "f()", "f(1, 'two')", "1 + 2", "-x", "!x", "~x", "(x)",
        "1 + 2 * 3 - 4 / 5 % 6 ** 7", "a << 1 >> 2 & 3 ^ 4 | 5", "a < b <= c > d >= e == f != g",
        "a && b || c", "null", "true", "false", "if(a, b, c)"
    };
    for (size_t ix = 0; ix < sizeof(expressions) / sizeof(expressions[0]); ix++) {
        BSExpr *expr = bsParseExpression(expressions[ix], strlen(expressions[ix]), 0, NULL, false, NULL);
        ASSERT_NOT_NULL(expr);
        BSValue model = bsExprToModel(expr);
        BSExpr *expr2 = bsExprFromModel(model);
        ASSERT_NOT_NULL(expr2);
        BSValue model2 = bsExprToModel(expr2);
        ASSERT_INT_EQ(bsValueCompare(model, model2), 0);
        bsRelease(model);
        bsRelease(model2);
        bsExprFree(expr);
        bsExprFree(expr2);
    }
}
