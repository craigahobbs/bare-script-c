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

    BSParserError error = {0};
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


/* Assert a streamed model's JSON is rejected with an error */
static void bsTestInvalidModelJSON(const char *json, const char *expectedError)
{
    const char *error = NULL;
    BSScript *script = bsScriptFromModelJSON(json, strlen(json), NULL, &error);
    ASSERT_NULL(script);
    ASSERT_STR_EQ(error, expectedError);
}


TEST(model_operand_limits)
{
    /* A chunk holds at most 32768 constants, and a call at most 32767 arguments */
    BSStringBuilder sb;
    bsSBInit(&sb);
    bsSBAppendString(&sb, "{\"statements\":[");
    for (int ix = 0; ix < 32800; ix++) {
        char statement[64];
        snprintf(statement, sizeof(statement), "%s{\"expr\":{\"name\":\"x\",\"expr\":{\"number\":%d.5}}}",
                 ix == 0 ? "" : ",", ix);
        bsSBAppendString(&sb, statement);
    }
    bsSBAppendString(&sb, "]}");
    BSValue json = bsSBToValue(&sb);
    bsTestInvalidModel(bsStringData(json));
    bsRelease(json);

    json = bsTestRepeat("{\"statements\":[{\"expr\":{\"expr\":{\"function\":{\"name\":\"f\",\"args\":[{\"variable\":\"x\"}",
                        ",{\"variable\":\"x\"}", 32767, "]}}}}]}");
    bsTestInvalidModel(bsStringData(json));
    bsRelease(json);

    /* 32768 live temporaries - a call's arguments hold theirs until the call, and a nested call adds more */
#define BS_TEST_BINARY "{\"binary\":{\"op\":\"+\",\"left\":{\"string\":\"a\"},\"right\":{\"string\":\"a\"}}}"
    json = bsTestRepeat("{\"statements\":[{\"expr\":{\"expr\":{\"function\":{\"name\":\"f\",\"args\":[", BS_TEST_BINARY ",", 32766,
                        "{\"function\":{\"name\":\"g\",\"args\":[" BS_TEST_BINARY "," BS_TEST_BINARY "," BS_TEST_BINARY "]}}]}}}}]}");
    bsTestInvalidModel(bsStringData(json));
    bsRelease(json);

    /* A chunk holds at most 32768 includes */
    json = bsTestRepeat("{\"statements\":[{\"include\":{\"includes\":[{\"url\":\"a.bare\"}", ",{\"url\":\"a.bare\"}", 32768, "]}}]}");
    bsTestInvalidModel(bsStringData(json));
    bsTestInvalidModelJSON(bsStringData(json), "Invalid BareScript model");
    bsRelease(json);

    /* A script defines at most 65536 functions */
    static const char *functionJSON = "{\"function\":{\"name\":\"f\",\"args\":[],\"statements\":[]}}";
    BSValue function = bsJSONDecode(functionJSON, strlen(functionJSON), NULL);
    BSValue model = bsObjectNew();
    BSValue statements = bsArrayNewCapacity(65537);
    for (int ix = 0; ix < 65537; ix++) {
        bsArrayPush(statements, bsRetain(function));
    }
    bsObjectSet(model, "statements", statements);
    bsRelease(function);
    ASSERT_NULL(bsScriptFromModel(model, NULL));
    bsRelease(model);

    /* A jump to an unknown label needs a constant for the label's name, after the last one was taken */
    bsSBInit(&sb);
    bsSBAppendString(&sb, "{\"statements\":[{\"jump\":{\"label\":\"nope\"}}");
    for (int ix = 0; ix < 32767; ix++) {
        char statement[64];
        snprintf(statement, sizeof(statement), ",{\"expr\":{\"expr\":{\"number\":%d.5}}}", ix);
        bsSBAppendString(&sb, statement);
    }
    bsSBAppendString(&sb, "]}");
    json = bsSBToValue(&sb);
    bsTestInvalidModel(bsStringData(json));
    bsTestInvalidModelJSON(bsStringData(json), "Invalid BareScript model");
    bsRelease(json);

    /* Malformed expression statements, operands, and a conditional's branch assigned to a local */
    bsTestInvalidModel("{\"statements\":[{\"expr\":{\"expr\":{\"function\":{\"name\":5}}}}]}");
    bsTestInvalidModel("{\"statements\":[{\"expr\":{\"expr\":{\"bogus\":1}}}]}");
    bsTestInvalidModel("{\"statements\":[{\"function\":{\"name\":\"f\",\"args\":[],\"statements\":"
                       "[{\"expr\":{\"name\":\"x\",\"expr\":{\"number\":\"bad\"}}}]}}]}");
    bsTestInvalidModel("{\"statements\":[{\"expr\":{\"expr\":{\"binary\":5}}}]}");
    bsTestInvalidModel("{\"statements\":[{\"expr\":{\"name\":\"x\",\"expr\":{\"number\":\"5\"}}}]}");
    bsTestInvalidModel("{\"statements\":[{\"expr\":{\"expr\":{\"unary\":{\"op\":\"-\",\"expr\":{\"bogus\":1}}}}}]}");
    bsTestInvalidModel("{\"statements\":[{\"function\":{\"name\":\"f\",\"args\":[],\"statements\":["
                       "{\"expr\":{\"name\":\"x\",\"expr\":{\"function\":{\"name\":\"if\","
                       "\"args\":[{\"number\":1},{\"bogus\":1}]}}}}]}}]}");
}


TEST(model_script_from_json)
{
    /* A streamed model compiles to the same script as the decoded model, without keeping it */
    static const char *json = "{\"statements\":[{\"function\":{\"name\":\"f\",\"args\":[\"x\"],\"statements\":["
        "{\"return\":{\"expr\":{\"binary\":{\"op\":\"+\",\"left\":{\"variable\":\"x\"},"
        "\"right\":{\"number\":1}}}}}]}},{\"return\":{\"expr\":{\"function\":{\"name\":\"f\","
        "\"args\":[{\"number\":2}]}}}}],\"scriptName\":\"stream.bare\",\"scriptLines\":[\"a\"],"
        "\"system\":true}";
    const char *error = "unset";
    BSScript *script = bsScriptFromModelJSON(json, strlen(json), NULL, &error);
    ASSERT_NOT_NULL(script);
    ASSERT_NULL(error);
    ASSERT_INT_EQ(script->model.type, BS_NULL);
    ASSERT_NULL(script->code.cover);
    ASSERT_TRUE(script->system);
    ASSERT_VALUE_STRING(bsRetain(script->scriptName), "stream.bare");
    ASSERT_INT_EQ(bsArrayCount(script->scriptLines), 1);
    ASSERT_INT_EQ(script->functionCount, 1);
    BSOptions *options = bsOptionsNew();
    ASSERT_VALUE(bsExecuteScript(script, options), "3");
    bsOptionsFree(options);
    bsScriptRelease(script);

    /* The caller's name wins, and an empty statements array is a valid script */
    static const char *named = "{\"statements\": [], \"scriptName\": \"model\"}";
    script = bsScriptFromModelJSON(named, strlen(named), "caller.bare", NULL);
    ASSERT_NOT_NULL(script);
    ASSERT_VALUE_STRING(bsRetain(script->scriptName), "caller.bare");
    ASSERT_FALSE(script->system);
    bsScriptRelease(script);

    /* Malformed JSON and malformed models */
    bsTestInvalidModelJSON("", "Expecting value");
    bsTestInvalidModelJSON("[]", "Expecting value");
    bsTestInvalidModelJSON("{", "Expecting property name enclosed in double quotes");
    bsTestInvalidModelJSON("{\"statements\" []}", "Expecting ':' delimiter");
    bsTestInvalidModelJSON("{\"statements\": 5}", "Invalid BareScript model");
    bsTestInvalidModelJSON("{\"scriptName\": }", "Expecting value");
    bsTestInvalidModelJSON("{}", "Invalid BareScript model");
    bsTestInvalidModelJSON("{\"statements\": [1]}", "Invalid BareScript model");
    bsTestInvalidModelJSON("{\"statements\": [{\"bogus\": 1}]}", "Invalid BareScript model");
    bsTestInvalidModelJSON("{\"statements\": [{\"return\": {}}", "Expecting ',' delimiter");
    bsTestInvalidModelJSON("{\"statements\": [{\"return\": {}},]}", "Illegal trailing comma before end of array");
    bsTestInvalidModelJSON("{\"statements\": [}", "Expecting value");
    bsTestInvalidModelJSON("{\"statements\": [],}", "Illegal trailing comma before end of object");
    bsTestInvalidModelJSON("{\"statements\": []} x", "Extra data");
    bsTestInvalidModelJSON("{\"statements\": [{5}]}", "Expecting property name enclosed in double quotes");
    bsTestInvalidModelJSON("{\"stat\\u0065ments\" []}", "Expecting ':' delimiter");
    bsTestInvalidModelJSON("{\"statements\": [{\"return\": {5}}]}", "Expecting property name enclosed in double quotes");
    bsTestInvalidModelJSON("{\"statements\": [{\"return\": {\"lineNumber\": 1 \"x\": 2}}]}", "Expecting ',' delimiter");
    bsTestInvalidModelJSON("{\"statements\": [{\"return\": {\"expr\": {5}}}]}",
                           "Expecting property name enclosed in double quotes");
    bsTestInvalidModelJSON("{\"statements\": [{\"return\": {\"expr\": {\"function\": {\"name\": \"f\", "
                           "\"args\": [{\"number\": }]}}}}]}", "Expecting value");
    bsTestInvalidModelJSON("{\"statements\": [{\"return\": {\"expr\": {\"function\": {\"name\": \"f\", "
                           "\"args\": [{\"number\": 1} {\"number\": 2}]}}}}]}", "Expecting ',' delimiter");
    bsTestInvalidModelJSON("{\"statements\": [{\"function\": {\"name\": \"f\", \"args\": [], "
                           "\"statements\": [{\"return\": {\"expr\": }}]}}]}", "Expecting value");
    bsTestInvalidModelJSON("{\"statements\": [{\"function\": {\"name\": \"f\", \"args\": [], "
                           "\"statements\": [], \"lastArgArray\": }}]}", "Expecting value");
    bsTestInvalidModelJSON("{\"statements\": [{\"expr\": {\"expr\": {\"binary\": {\"op\": \"+\", "
                           "\"left\": {\"number\": 1}, \"right\": {\"number\": 2}, \"x\": }}}}]}", "Expecting value");
    bsTestInvalidModelJSON("{\"state\\qments\": []}", "Invalid \\escape");
    bsTestInvalidModelJSON("{\"statements\": [{\"return\": {\"lineNumber\": }}]}", "Expecting value");
    bsTestInvalidModelJSON("{\"statements\": [{\"return\": {\"expr\": {\"function\": {\"name\": \"f\", \"args\": "
                           "[{\"function\": {\"name\": \"g\", \"args\": [{\"number\": }]}}]}}}}]}", "Expecting value");
    bsTestInvalidModelJSON("{\"statements\": [{\"include\": {\"includes\": [{\"url\": \"a\", \"system\": }]}}]}",
                           "Expecting value");
    bsTestInvalidModelJSON("{\"statements\": [{\"function\": {\"name\": \"f\", \"args\": [\"x\" \"y\"], "
                           "\"statements\": []}}]}", "Expecting ',' delimiter");
}


/* Assert a streamed model's JSON compiles, returning the script */
static BSScript *bsTestModelJSON(const char *json)
{
    const char *error = NULL;
    BSScript *script = bsScriptFromModelJSON(json, strlen(json), NULL, &error);
    if (script == NULL) {
        bsTestFail(__FILE__, __LINE__, "bsScriptFromModelJSON(%s) failed: %s", json, error);
    }
    return script;
}


/* Assert a streamed model's JSON compiles and executes to the expected JSON result */
static void bsTestModelJSONResult(const char *json, const char *expected)
{
    BSScript *script = bsTestModelJSON(json);
    if (script == NULL) {
        return;
    }
    BSOptions *options = bsOptionsNew();
    ASSERT_VALUE(bsExecuteScript(script, options), expected);
    bsOptionsFree(options);
    bsScriptRelease(script);
}


TEST(model_script_from_json_shapes)
{
    /* Each statement kind, its members, and the members a statement or expression ignores */
    bsTestModelJSONResult(
        "{\"stat\\u0065ments\": ["
        "{\"expr\": {\"name\": 5, \"expr\": {\"number\": 1}, \"lineNumber\": \"x\"}},"
        "{\"expr\": {\"name\": \"a\", \"expr\": {\"function\": {\"name\": \"arrayNew\", \"args\": 5, \"extra\": 1}}}},"
        "{\"expr\": {\"name\": \"b\", \"expr\": {\"binary\": {\"op\": \"+\", \"left\": {\"number\": 1}, "
        "\"right\": {\"number\": 2}, \"x\": 1}}}},"
        "{\"expr\": {\"name\": \"c\", \"expr\": {\"unary\": {\"op\": \"-\", \"expr\": {\"number\": 3}, \"x\": 1}}}},"
        "{\"function\": {\"name\": \"f\", \"args\": 5, \"statements\": [{\"return\": {\"expr\": {\"number\": 4}}}], "
        "\"lastArgArray\": 0, \"lineCount\": 1}},"
        "{\"function\": {\"name\": \"g\", \"args\": [\"x\", \"y\"], \"statements\": [{\"return\": {\"expr\": "
        "{\"function\": {\"name\": \"if\", \"args\": [{\"variable\": \"x\"}, {\"variable\": \"y\"}, "
        "{\"number\": 0}, {\"bogus\": 1}, {\"number\": 9}]}}}}], \"lastArgArray\": true}},"
        "{\"jump\": {\"label\": \"skip\", \"expr\": {\"variable\": \"true\"}, \"lineNumber\": 5}},"
        "{\"return\": {\"expr\": {\"number\": 0}}},"
        "{\"label\": {\"name\": \"skip\", \"lineNumber\": 6}},"
        "{\"return\": {\"expr\": {\"function\": {\"name\": \"arrayNew\", \"args\": [{\"variable\": \"a\"}, "
        "{\"variable\": \"b\"}, {\"variable\": \"c\"}, {\"function\": {\"name\": \"f\"}}, "
        "{\"function\": {\"name\": \"g\", \"args\": [{\"number\": 1}, {\"number\": 7}]}}]}}}}"
        "]}",
        "[[],3,-3,4,[7]]");

    /* A conditional's arguments past its third are not read, so their shapes are not checked - any other function's are */
    bsTestModelJSONResult(
        "{\"statements\": [{\"return\": {\"expr\": {\"function\": {\"name\": \"if\", \"args\": "
        "[{\"number\": 1}, {\"number\": 2}, {\"number\": 3}, {\"bogus\": 1}]}}}}]}", "2");
    bsTestInvalidModelJSON("{\"statements\": [{\"return\": {\"expr\": {\"function\": {\"name\": \"if\", \"args\": "
                           "[{\"number\": 1}, {\"bogus\": 1}, {\"number\": 3}, {\"number\": 4}]}}}}]}",
                           "Invalid BareScript model");
    bsTestInvalidModelJSON("{\"statements\": [{\"return\": {\"expr\": {\"function\": {\"name\": \"f\", \"args\": "
                           "[{\"number\": 1}, {\"number\": 2}, {\"number\": 3}, {\"bogus\": 1}]}}}}]}",
                           "Invalid BareScript model");

    /* An include statement's includes */
    BSScript *script = bsTestModelJSON(
        "{\"statements\": [{\"include\": {\"includes\": [{\"url\": \"a.bare\", \"extra\": 1}, "
        "{\"url\": \"b.bare\", \"system\": true}], \"lineNumber\": 1}}]}");
    ASSERT_NOT_NULL(script);
    ASSERT_INT_EQ(script->code.includeCount, 2);
    ASSERT_VALUE_STRING(bsRetain(script->code.includes[1].url), "b.bare");
    ASSERT_TRUE(script->code.includes[1].system);
    ASSERT_FALSE(script->code.includes[0].system);
    bsScriptRelease(script);

    /* Malformed statements */
    static const char *const invalid[] = {
        "{\"statements\": [{\"expr\": 5}]}",
        "{\"statements\": [{\"expr\": {}}]}",
        "{\"statements\": [{\"expr\": {\"expr\": {\"bogus\": {}}}}]}",
        "{\"statements\": [{\"expr\": {\"expr\": {\"number\": 1}, \"x\": 1}, \"y\": 1}]}",
        "{\"statements\": [{\"jump\": {\"label\": 5}}]}",
        "{\"statements\": [{\"jump\": {\"expr\": {\"number\": 1}}}]}",
        "{\"statements\": [{\"jump\": {\"label\": \"x\", \"expr\": {\"bogus\": {}}}}]}",
        "{\"statements\": [{\"return\": {\"expr\": 5}}]}",
        "{\"statements\": [{\"return\": {\"expr\": {\"bogus\": 1}}}]}",
        "{\"statements\": [{\"label\": {\"name\": 5}}]}",
        "{\"statements\": [{\"label\": {}}]}",
        "{\"statements\": [{\"function\": {\"args\": [], \"statements\": []}}]}",
        "{\"statements\": [{\"function\": {\"name\": \"f\", \"args\": []}}]}",
        "{\"statements\": [{\"function\": {\"name\": \"f\", \"args\": [5], \"statements\": []}}]}",
        "{\"statements\": [{\"function\": {\"name\": \"f\", \"args\": [], \"statements\": 5}}]}",
        "{\"statements\": [{\"function\": {\"name\": \"f\", \"args\": [], \"statements\": [{\"bogus\": {}}]}}]}",
        "{\"statements\": [{\"include\": {}}]}",
        "{\"statements\": [{\"include\": {\"includes\": 5}}]}",
        "{\"statements\": [{\"include\": {\"includes\": []}}]}",
        "{\"statements\": [{\"include\": {\"includes\": [5]}}]}",
        "{\"statements\": [{\"include\": {\"includes\": [{}]}}]}",
        "{\"statements\": [{\"include\": {\"includes\": [{\"url\": 5}]}}]}",
        /* Malformed expressions */
        "{\"statements\": [{\"return\": {\"expr\": {\"number\": \"5\"}}}]}",
        "{\"statements\": [{\"return\": {\"expr\": {\"number\": 1e999}}}]}",
        "{\"statements\": [{\"return\": {\"expr\": {\"string\": 5}}}]}",
        "{\"statements\": [{\"return\": {\"expr\": {\"variable\": 5}}}]}",
        "{\"statements\": [{\"return\": {\"expr\": {\"group\": {\"bogus\": 1}}}}]}",
        "{\"statements\": [{\"return\": {\"expr\": {\"function\": {\"args\": []}}}}]}",
        "{\"statements\": [{\"return\": {\"expr\": {\"function\": {\"name\": 5}}}}]}",
        "{\"statements\": [{\"return\": {\"expr\": {\"function\": 5}}}]}",
        "{\"statements\": [{\"return\": {\"expr\": {\"binary\": {\"op\": \"+\"}}}}]}",
        "{\"statements\": [{\"return\": {\"expr\": {\"binary\": {\"op\": 5, \"left\": {\"number\": 1}, "
        "\"right\": {\"number\": 1}}}}}]}",
        "{\"statements\": [{\"return\": {\"expr\": {\"binary\": {\"op\": \"??\", \"left\": {\"number\": 1}, "
        "\"right\": {\"number\": 1}}}}}]}",
        "{\"statements\": [{\"return\": {\"expr\": {\"binary\": {\"op\": \"<<<\", \"left\": {\"number\": 1}, "
        "\"right\": {\"number\": 1}}}}}]}",
        "{\"statements\": [{\"return\": {\"expr\": {\"binary\": {\"op\": \"+-\", \"left\": {\"number\": 1}, "
        "\"right\": {\"number\": 1}}}}}]}",
        "{\"statements\": [{\"return\": {\"expr\": {\"binary\": {\"op\": \"+\", \"left\": {\"bogus\": 1}, "
        "\"right\": {\"number\": 1}}}}}]}",
        "{\"statements\": [{\"return\": {\"expr\": {\"unary\": {\"op\": \"-\"}}}}]}",
        "{\"statements\": [{\"return\": {\"expr\": {\"unary\": {\"op\": 5, \"expr\": {\"number\": 1}}}}}]}",
        "{\"statements\": [{\"return\": {\"expr\": {\"unary\": {\"op\": \"?\", \"expr\": {\"number\": 1}}}}}]}",
        "{\"statements\": [{\"return\": {\"expr\": {\"unary\": {\"op\": \"-\", \"expr\": {\"bogus\": 1}}}}}]}",
        "{\"statements\": [{\"return\": {\"expr\": {\"number\": 1, \"x\": 1}}}]}",
        "{\"statements\": [{\"return\": {\"expr\": {\"bogus\": {}}}}]}"
    };
    for (size_t ix = 0; ix < sizeof(invalid) / sizeof(invalid[0]); ix++) {
        bsTestInvalidModelJSON(invalid[ix], "Invalid BareScript model");
    }

    /* Nesting past the decoder's depth - at an expression, at a node's members, at an argument array */
    static const struct {
        int groups;
        const char *inner;
    } deep[] = {
        {1000, "{\"number\": 1}"},
        {995, "{\"unary\": {\"op\": \"-\", \"expr\": {\"number\": 1}}}"},
        {994, "{\"function\": {\"name\": \"f\", \"args\": [{\"number\": 1}]}}"}
    };
    for (size_t ix = 0; ix < sizeof(deep) / sizeof(deep[0]); ix++) {
        BSValue open = bsTestRepeat("{\"statements\": [{\"return\": {\"expr\": ", "{\"group\": ", (size_t) deep[ix].groups,
                                    deep[ix].inner);
        BSValue close = bsTestRepeat("", "}", (size_t) deep[ix].groups, "}}]}");
        BSValue json = bsStringConcat(open, close);
        bsTestInvalidModelJSON(bsStringData(json), "Maximum nesting depth exceeded");
        bsRelease(json);
        bsRelease(open);
        bsRelease(close);
    }
}


TEST(model_from_model_shapes)
{
    /* A conditional's arguments past its third are not read, so a malformed one stands */
    ASSERT_VALUE(bsTestExecute("return if(1, 2, 3, 4, 5)"), "2");
    BSScript *script = bsTestScriptFromJSON(
        "{\"statements\": [{\"return\": {\"expr\": {\"function\": {\"name\": \"if\", "
        "\"args\": [{\"number\": 1}, {\"number\": 2}, {\"number\": 3}, {\"bogus\": 1}]}}}}]}");
    ASSERT_NOT_NULL(script);
    BSOptions *options = bsOptionsNew();
    ASSERT_VALUE(bsExecuteScript(script, options), "2");
    bsOptionsFree(options);
    bsScriptRelease(script);

    /* Malformed models */
    bsTestInvalidModel("{\"statements\": [{\"return\": {\"expr\": {\"variable\": 5}}}]}");
    bsTestInvalidModel("{\"statements\": [{\"return\": {\"expr\": {\"bogus\": {}}}}]}");
    bsTestInvalidModel("{\"statements\": [{\"return\": {\"expr\": {\"function\": {\"name\": \"f\", "
                       "\"args\": [{\"bogus\": 1}]}}}}]}");
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

    /* Fail after emit has already allocated includes, slots, or jump patches */
    bsTestInvalidModel("{\"statements\":[{\"include\":{\"includes\":[{\"url\":\"a.bare\"}]}},{}]}");
    bsTestInvalidModel("{\"statements\":[{\"function\":{\"name\":\"f\",\"args\":[\"x\"],\"statements\":[{}]}}]}");
    bsTestInvalidModel("{\"statements\":[{\"jump\":{\"label\":\"later\"}},{}]}");
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
    ASSERT_DOUBLE_EQ(bsObjectGet(bsObjectGet(bsArrayGet(bsObjectGet(script->model, "statements"), 0),
                                            "expr"), "lineNumber").u.number, 1);
    ASSERT_DOUBLE_EQ(bsObjectGet(bsObjectGet(bsArrayGet(bsObjectGet(script->model, "statements"), 0),
                                            "expr"), "lineCount").u.number, 2);

    BSOptions *options = bsTestOptions();
    ASSERT_VALUE(bsExecuteScript(script, options), "[2]");
    bsOptionsFree(options);
    bsScriptRelease(script);

    /* An include statement's system flag round-trips */
    script = bsTestScriptFromJSON(
        "{\"statements\":[{\"include\":{\"includes\":["
        "{\"url\":\"a.bare\"},{\"url\":\"b.bare\",\"system\":true}]}}]}");
    ASSERT_NOT_NULL(script);
    ASSERT_INT_EQ((int) script->code.includeCount, 2);
    ASSERT_FALSE(script->code.includes[0].system);
    ASSERT_TRUE(script->code.includes[1].system);
    BSValue model = bsScriptToModel(script);
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


TEST(model_statement_list_form)
{
    /* A statement object whose entries moved to the heap and shrank back to one member is still a node */
    static const char *returnJSON = "{\"expr\":{\"number\":7}}";
    BSValue statement = bsObjectNew();
    bsTestObjectFill(statement, "extra%d", 0, 5);
    bsObjectSet(statement, "return", bsJSONDecode(returnJSON, strlen(returnJSON), NULL));
    for (int ix = 0; ix < 5; ix++) {
        char key[16];
        snprintf(key, sizeof(key), "extra%d", ix);
        ASSERT_TRUE(bsObjectDelete(statement, key));
    }
    ASSERT_INT_EQ(bsObjectCount(statement), 1);
    ASSERT_TRUE(statement.u.object->entries != statement.u.object->inline_);
    BSValue statements = bsArrayNew();
    bsArrayPush(statements, statement);
    BSValue model = bsObjectNew();
    bsObjectSet(model, "statements", statements);

    BSScript *script = bsScriptFromModel(model, NULL);
    ASSERT_NOT_NULL(script);
    BSOptions *options = bsTestOptions();
    ASSERT_VALUE(bsExecuteScript(script, options), "7");
    bsOptionsFree(options);
    bsScriptRelease(script);
    bsRelease(model);
}
