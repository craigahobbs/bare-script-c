/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The JSON encoder/decoder unit tests
 */

#include <string.h>

#include "test.h"


/* Decode JSON text, asserting success */
static BSValue bsTestJSON(const char *text)
{
    const char *error = NULL;
    BSValue value = bsJSONDecode(text, strlen(text), &error);
    if (error != NULL) {
        bsTestFail(__FILE__, __LINE__, "bsJSONDecode(%s) failed: %s", text, error);
    }
    return value;
}


/* Decode JSON text, asserting failure with the given error */
static void bsTestJSONError(const char *text, const char *expectedError)
{
    const char *error = NULL;
    BSValue value = bsJSONDecode(text, strlen(text), &error);
    bsTestAssertEqual(__FILE__, __LINE__, text, error, expectedError);
    bsRelease(value);
}


TEST(json_encode_deep_indent)
{
    /* Indentation deeper than one chunk of spaces is appended in pieces */
    BSValue inner = bsArrayNew();
    bsArrayPush(inner, bsNumber(1));
    BSValue value = inner;
    for (int depth = 0; depth < 20; depth++) {
        BSValue outer = bsArrayNew();
        bsArrayPush(outer, value);
        value = outer;
    }
    BSValue json = bsJSONEncode(value, 4);

    /* The innermost value is at depth 21, so its line is indented by 84 spaces */
    char needle[88];
    needle[0] = '\n';
    memset(needle + 1, ' ', 84);
    memcpy(needle + 85, "1\n", 3);
    ASSERT_STR_CONTAINS(bsStringData(json), needle);
    bsRelease(json);
    bsRelease(value);
}


TEST(json_encode_scalars)
{
    ASSERT_VALUE_STRING(bsJSONEncode(bsNull(), 0), "null");
    ASSERT_VALUE_STRING(bsJSONEncode(bsBoolean(true), 0), "true");
    ASSERT_VALUE_STRING(bsJSONEncode(bsBoolean(false), 0), "false");
    ASSERT_VALUE_STRING(bsJSONEncode(bsNumber(1.5), 0), "1.5");
    ASSERT_VALUE_STRING(bsJSONEncode(bsNumber(1.0 / 0.0), 0), "null");
    ASSERT_VALUE_STRING(bsJSONEncode(bsStringNew("abc"), 0), "\"abc\"");
}


TEST(json_encode_string_escapes)
{
    ASSERT_VALUE_STRING(bsJSONEncode(bsStringNew("a\"b"), 0), "\"a\\\"b\"");
    ASSERT_VALUE_STRING(bsJSONEncode(bsStringNew("a\\b"), 0), "\"a\\\\b\"");
    ASSERT_VALUE_STRING(bsJSONEncode(bsStringNew("a\nb"), 0), "\"a\\nb\"");
    ASSERT_VALUE_STRING(bsJSONEncode(bsStringNew("a\rb"), 0), "\"a\\rb\"");
    ASSERT_VALUE_STRING(bsJSONEncode(bsStringNew("a\tb"), 0), "\"a\\tb\"");
    ASSERT_VALUE_STRING(bsJSONEncode(bsStringNew("a\bb"), 0), "\"a\\bb\"");
    ASSERT_VALUE_STRING(bsJSONEncode(bsStringNew("a\fb"), 0), "\"a\\fb\"");
    ASSERT_VALUE_STRING(bsJSONEncode(bsStringNewSize("a\x01"
                                              "b", 3), 0), "\"a\\u0001b\"");

    /* Non-ASCII characters are not escaped */
    ASSERT_VALUE_STRING(bsJSONEncode(bsStringNew("\xc3\xa9"), 0), "\"\xc3\xa9\"");
}


TEST(json_encode_containers)
{
    BSValue array = bsArrayNew();
    ASSERT_VALUE_KEEP(array, "[]");
    bsArrayPush(array, bsNumber(1));
    bsArrayPush(array, bsStringNew("two"));
    ASSERT_VALUE_KEEP(array, "[1,\"two\"]");
    ASSERT_VALUE_STRING(bsJSONEncode(array, 2), "[\n  1,\n  \"two\"\n]");
    bsRelease(array);

    BSValue object = bsObjectNew();
    ASSERT_VALUE_KEEP(object, "{}");
    bsObjectSet(object, "z", bsNumber(1));
    bsObjectSet(object, "a", bsNumber(2));
    ASSERT_VALUE_KEEP(object, "{\"a\":2,\"z\":1}");
    ASSERT_VALUE_STRING(bsJSONEncode(object, 4), "{\n    \"a\": 2,\n    \"z\": 1\n}");
    bsRelease(object);
}


TEST(json_encode_other_types)
{
    /* A datetime encodes as its string representation */
    int64_t milliseconds = 0;
    ASSERT_TRUE(bsDatetimeFromParts(2026, 8, 6, 0, 0, 0, 0, &milliseconds));
    BSValue datetime = bsDatetime(milliseconds);
    BSValue text = bsValueString(datetime);
    BSValue expected = bsStringNewFormat("\"%s\"", bsStringData(text));
    ASSERT_VALUE_STRING(bsJSONEncode(datetime, 0), bsStringData(expected));
    bsRelease(text);
    bsRelease(expected);

    /* A regex has no JSON representation */
    BSValue regex = bsRegexNew("a", 1, 0, NULL, 0);
    ASSERT_VALUE_STRING(bsJSONEncode(regex, 0), "null");
    bsRelease(regex);
}


TEST(json_encode_depth)
{
    /* Nesting past the depth limit encodes as null */
    BSValue root = bsArrayNew();
    BSValue current = root;
    for (int ix = 0; ix < 1100; ix++) {
        BSValue next = bsArrayNew();
        bsArrayPush(current, next);
        current = next;
    }
    BSValue json = bsJSONEncode(root, 0);
    ASSERT_STR_CONTAINS(bsStringData(json), "null");
    bsRelease(json);
    bsRelease(root);
}


TEST(json_decode_scalars)
{
    ASSERT_VALUE(bsTestJSON("null"), "null");
    ASSERT_VALUE(bsTestJSON("true"), "true");
    ASSERT_VALUE(bsTestJSON("false"), "false");
    ASSERT_VALUE(bsTestJSON("0"), "0");
    ASSERT_VALUE(bsTestJSON("-1.5"), "-1.5");
    ASSERT_VALUE(bsTestJSON("1e3"), "1000");
    ASSERT_VALUE(bsTestJSON("1E+3"), "1000");
    ASSERT_VALUE(bsTestJSON("1e-3"), "0.001");
    ASSERT_VALUE(bsTestJSON("1e999"), "null");
    ASSERT_VALUE(bsTestJSON("[-1e999, {\"a\": 1e999}]"), "[null,{\"a\":null}]");
    ASSERT_VALUE(bsTestJSON("  1  "), "1");
    ASSERT_VALUE(bsTestJSON("\t\r\n1"), "1");
    ASSERT_VALUE(bsTestJSON("\"abc\""), "\"abc\"");
}


TEST(json_decode_string_escapes)
{
    ASSERT_VALUE(bsTestJSON("\"a\\\"b\""), "\"a\\\"b\"");
    ASSERT_VALUE(bsTestJSON("\"a\\\\b\""), "\"a\\\\b\"");
    ASSERT_VALUE(bsTestJSON("\"a\\/b\""), "\"a/b\"");
    ASSERT_VALUE(bsTestJSON("\"\\b\\f\\n\\r\\t\""), "\"\\b\\f\\n\\r\\t\"");
    ASSERT_VALUE(bsTestJSON("\"\\u0041\""), "\"A\"");
    ASSERT_VALUE(bsTestJSON("\"\\u00e9\""), "\"\xc3\xa9\"");
    ASSERT_VALUE(bsTestJSON("\"\\uD83D\\uDE00\""), "\"\xf0\x9f\x98\x80\"");

    /* An unpaired surrogate becomes the replacement character, keeping the string valid UTF-8 */
    ASSERT_VALUE(bsTestJSON("\"\\uD83Dx\""), "\"\xef\xbf\xbd" "x\"");
    ASSERT_VALUE(bsTestJSON("\"\\uD83D\\u0041\""), "\"\xef\xbf\xbd" "A\"");
    ASSERT_VALUE(bsTestJSON("\"\\uDE00\""), "\"\xef\xbf\xbd\"");
}


TEST(json_decode_containers)
{
    ASSERT_VALUE(bsTestJSON("[]"), "[]");
    ASSERT_VALUE(bsTestJSON("[ ]"), "[]");
    ASSERT_VALUE(bsTestJSON("[1,2,3]"), "[1,2,3]");
    ASSERT_VALUE(bsTestJSON("[1, [2, {\"a\": null}]]"), "[1,[2,{\"a\":null}]]");
    ASSERT_VALUE(bsTestJSON("{}"), "{}");
    ASSERT_VALUE(bsTestJSON("{ }"), "{}");
    ASSERT_VALUE(bsTestJSON("{\"b\":1,\"a\":2}"), "{\"a\":2,\"b\":1}");
    ASSERT_VALUE(bsTestJSON("{ \"a\" : 1 }"), "{\"a\":1}");

    /* A duplicate key keeps the last value */
    ASSERT_VALUE(bsTestJSON("{\"a\":1,\"a\":2}"), "{\"a\":2}");
}


TEST(json_decode_errors)
{
    bsTestJSONError("", "Expecting value");
    bsTestJSONError("  ", "Expecting value");
    bsTestJSONError("x", "Expecting value");
    bsTestJSONError("tru", "Expecting value");
    bsTestJSONError("fals", "Expecting value");
    bsTestJSONError("nul", "Expecting value");
    bsTestJSONError("1 2", "Extra data");
    bsTestJSONError("-", "Expecting value");
    bsTestJSONError("1.", "Extra data");
    bsTestJSONError("1e", "Extra data");
    bsTestJSONError("1e+", "Extra data");
    bsTestJSONError("\"abc", "Unterminated string starting at");
    bsTestJSONError("\"a\\", "Unterminated string starting at");
    bsTestJSONError("\"a\\q\"", "Invalid \\escape");
    bsTestJSONError("\"a\\u00\"", "Invalid \\uXXXX escape");
    bsTestJSONError("\"a\\u\"", "Invalid \\uXXXX escape");
    bsTestJSONError("\"\x01\"", "Invalid control character at");
    bsTestJSONError("[1", "Expecting ',' delimiter");
    bsTestJSONError("[1 2]", "Expecting ',' delimiter");
    bsTestJSONError("[1,]", "Illegal trailing comma before end of array");
    bsTestJSONError("{1:2}", "Expecting property name enclosed in double quotes");
    bsTestJSONError("{\"a", "Unterminated string starting at");
    bsTestJSONError("{\"a\"}", "Expecting ':' delimiter");
    bsTestJSONError("{\"a\"", "Expecting ':' delimiter");
    bsTestJSONError("{\"a\":1", "Expecting ',' delimiter");
    bsTestJSONError("{\"a\":1 \"b\":2}", "Expecting ',' delimiter");
    bsTestJSONError("{\"a\":}", "Expecting value");
    bsTestJSONError("{\"a\":1,}", "Illegal trailing comma before end of object");
    bsTestJSONError("-x", "Expecting value");
    bsTestJSONError("[[[[", "Expecting value");

    /* A number too long for the decoder's parse buffer, which strtod reads in place */
    char big[128];
    memset(big, '1', sizeof(big));
    const char *error = NULL;
    BSValue bigValue = bsJSONDecode(big, sizeof(big), &error);
    ASSERT_NULL(error);
    ASSERT_TRUE(bigValue.u.number > 1e126 && bigValue.u.number < 1e128);
    bsRelease(bigValue);

    /* The nesting depth limit */
    BSValue text = bsTestRepeat(NULL, "[", 1100, NULL);
    error = NULL;
    bsRelease(bsJSONDecode(bsStringData(text), bsStringSize(text), &error));
    ASSERT_STR_EQ(error, "Maximum nesting depth exceeded");
    bsRelease(text);

    /* The error argument is optional */
    bsRelease(bsJSONDecode("x", 1, NULL));
    bsRelease(bsJSONDecode("1", 1, NULL));
    bsRelease(bsJSONDecode("1 2", 3, NULL));
}


TEST(json_decode_invalid_hex)
{
    /* Four characters are available, but one is not a hex digit */
    bsTestJSONError("\"\\u00zz\"", "Invalid \\uXXXX escape");
    bsTestJSONError("\"\\u0G00\"", "Invalid \\uXXXX escape");
    ASSERT_VALUE(bsTestJSON("\"\\u00FF\""), "\"\xc3\xbf\"");
    ASSERT_VALUE(bsTestJSON("\"\\u00ff\""), "\"\xc3\xbf\"");
}


TEST(json_decode_error_offset)
{
    /* The decoder reports where it detected the error */
    const char *error = NULL;
    size_t offset = 0;
    bsRelease(bsJSONDecodeEx("   x", 4, &error, &offset));
    ASSERT_STR_EQ(error, "Expecting value");
    ASSERT_INT_EQ(offset, 3);

    bsRelease(bsJSONDecodeEx("[1] x", 5, &error, &offset));
    ASSERT_STR_EQ(error, "Extra data");
    ASSERT_INT_EQ(offset, 4);

    /* The offset argument is optional, and is untouched on success */
    error = NULL;
    bsRelease(bsJSONDecodeEx("1", 1, &error, NULL));
    ASSERT_NULL(error);
}
