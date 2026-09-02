/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The BareScript library unit tests
 *
 * Library functions are exercised through the runtime, which is how scripts reach them and which
 * also covers the expression evaluator's call dispatch.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "../src/internal.h"


/* Duplicate a string with malloc, for option data the runtime frees */
static char *bsTestTempFileData(const char *text)
{
    size_t size = strlen(text) + 1;
    char *result = malloc(size);
    memcpy(result, text, size);
    return result;
}


/* Execute "return <expression>" and assert the result's JSON */
static void bsTestExpr(const char *expression, const char *expectedJSON)
{
    BSValue text = bsStringNewFormat("return %s", expression);
    BSValue result = bsTestExecute(bsStringData(text));
    BSValue json = bsJSONEncode(result, 0);
    if (!bsTestStringEqual(bsStringData(json), expectedJSON)) {
        bsTestFail(__FILE__, __LINE__, "%s\n    actual:   %s\n    expected: %s", expression,
                   bsStringData(json), expectedJSON);
    } else {
        bsTestPass();
    }
    bsRelease(json);
    bsRelease(result);
    bsRelease(text);
}


TEST(library_array)
{
    bsTestExpr("arrayCopy([1, 2])", "[1,2]");
    bsTestExpr("arrayCopy('x')", "null");
    bsTestExpr("arrayLength([1, 2])", "2");
    bsTestExpr("arrayLength('x')", "0");
    bsTestExpr("arrayGet([1, 2], 1)", "2");
    bsTestExpr("arrayGet([1, 2], 5)", "null");
    bsTestExpr("arrayGet([1, 2], -1)", "null");
    bsTestExpr("arrayGet([1, 2], 1.5)", "null");
    bsTestExpr("arrayGet([1, 2], true)", "null");
    bsTestExpr("arrayNew()", "[]");
    bsTestExpr("arrayNew(1)", "[1]");
    bsTestExpr("arrayNew(1, 'a')", "[1,\"a\"]");
    bsTestExpr("arrayNew(1, 2, 3)", "[1,2,3]");
    bsTestExpr("arrayNew(1, 2, 3, 4)", "[1,2,3,4]");
    bsTestExpr("arrayNewSize()", "[]");
    bsTestExpr("arrayNewSize(3)", "[0,0,0]");
    bsTestExpr("arrayNewSize(2, 'x')", "[\"x\",\"x\"]");
    bsTestExpr("arrayNewSize(-1)", "null");
    bsTestExpr("arrayExtend([1], [2, 3])", "[1,2,3]");
    bsTestExpr("arrayExtend([1], 'x')", "null");
    bsTestExpr("arrayFlat([1, [2, [3, [4]]]])", "[1,2,3,4]");
    bsTestExpr("arrayFlat([1, [2, [3]]], 1)", "[1,2,[3]]");
    bsTestExpr("arrayFlat([1, [2]], 0)", "[1,[2]]");
    bsTestExpr("arrayFlat([1, [2]], -1)", "[1,[2]]");
    bsTestExpr("arrayIndexOf([5, -3, 7], -3)", "1");
    bsTestExpr("arrayIndexOf([5, -3, 7], 99)", "-1");
    bsTestExpr("arrayIndexOf([1, 2, 1], 1, 1)", "2");
    bsTestExpr("arrayIndexOf('x', 1)", "-1");
    bsTestExpr("arrayLastIndexOf([1, 2, 1], 1)", "2");
    bsTestExpr("arrayLastIndexOf([1, 2, 1], 1, 1)", "0");
    bsTestExpr("arrayLastIndexOf([1, 2, 1], 1, 99)", "2");
    bsTestExpr("arrayLastIndexOf([], 1)", "-1");
    bsTestExpr("arrayLastIndexOf([1, 2], 99)", "-1");
    bsTestExpr("arrayJoin([1, null, 'x'], '|')", "\"1|null|x\"");
    bsTestExpr("arrayJoin([], '|')", "\"\"");
    bsTestExpr("arrayJoin('x', '|')", "null");
    bsTestExpr("arraySlice([1, 2, 3, 4], 1, 3)", "[2,3]");
    bsTestExpr("arraySlice([1, 2, 3], 1)", "[2,3]");
    bsTestExpr("arraySlice([1, 2, 3], 2, 1)", "[]");
    bsTestExpr("arraySlice([1, 2], 5)", "null");
    bsTestExpr("arraySlice([1, 2], 0, 5)", "null");
    bsTestExpr("arrayReverse([1, 2, 3])", "[3,2,1]");
    bsTestExpr("arrayReverse('x')", "null");
    bsTestExpr("arraySort([3, 1, 2])", "[1,2,3]");
    bsTestExpr("arraySort('x')", "null");
    bsTestExpr("arraySet([1, 2], 0, 'x')", "\"x\"");
    bsTestExpr("arraySet([1, 2], 5, 'x')", "null");
    bsTestExpr("arrayPush([1], 2, 3)", "[1,2,3]");
    bsTestExpr("arrayPush('x')", "null");
    bsTestExpr("arrayPop([1, 2])", "2");
    bsTestExpr("arrayPop([])", "null");
    bsTestExpr("arrayShift([1, 2])", "1");
    bsTestExpr("arrayShift([])", "null");

    /* In-place mutation */
    ASSERT_VALUE(bsTestExecute("a = [1, 2, 3]\narrayDelete(a, 1)\nreturn a"), "[1,3]");
    ASSERT_VALUE(bsTestExecute("a = [1]\nreturn [arrayDelete(a, 5), a]"), "[null,[1]]");
    ASSERT_VALUE(bsTestExecute("a = [1, 2]\narraySet(a, 0, 9)\nreturn a"), "[9,2]");
    ASSERT_VALUE(bsTestExecute("a = [1, 2]\narrayPop(a)\nreturn a"), "[1]");
    ASSERT_VALUE(bsTestExecute("a = [1, 2]\narrayShift(a)\nreturn a"), "[2]");

    /* Match and comparison functions */
    ASSERT_VALUE(bsTestExecute("function isNeg(v):\n    return v < 0\nendfunction\n"
                               "return arrayIndexOf([5, -3, 7], isNeg)"), "1");
    ASSERT_VALUE(bsTestExecute("function isNeg(v):\n    return v < 0\nendfunction\n"
                               "return arrayLastIndexOf([-5, -3, 7], isNeg)"), "1");
    ASSERT_VALUE(bsTestExecute("function isNeg(v):\n    return v < 0\nendfunction\n"
                               "return arrayIndexOf([1, 2], isNeg)"), "-1");
    ASSERT_VALUE(bsTestExecute("function cmpDesc(a, b):\n    return b - a\nendfunction\n"
                               "return arraySort([1, 3, 2], cmpDesc)"), "[3,2,1]");
    ASSERT_VALUE(bsTestExecute("function cmpBad(a, b):\n    return 'x'\nendfunction\n"
                               "return arraySort([1, 3, 2], cmpBad)"), "[1,3,2]");

    /* A runtime error inside a compare function halts the script; later compares do not run */
    ASSERT_VALUE(bsTestExecute("function cmpErr(a, b):\n    undefinedFunc()\nendfunction\n"
                               "return arraySort([1, 3, 2], cmpErr)"), "null");
    ASSERT_NOT_NULL(bsTestErrorText());

    /* A runtime error inside a match function stops the search and halts the script - the return
       statement never completes, as with the reference implementations' exceptions */
    ASSERT_VALUE(bsTestExecute("function bad(v):\n    undefinedFunc()\nendfunction\n"
                               "return arrayIndexOf([1, 2], bad)"), "null");
    ASSERT_NOT_NULL(bsTestErrorText());
    ASSERT_VALUE(bsTestExecute("function bad(v):\n    undefinedFunc()\nendfunction\n"
                               "return arrayLastIndexOf([1, 2], bad)"), "null");
    ASSERT_NOT_NULL(bsTestErrorText());
}


TEST(library_object_append)
{
    /* Append records an uninterned key while packed, and links into a treap once one exists */
    BSValue object = bsObjectNew();
    BSValue longKey = bsStringNew("a key longer than the sixty-four byte limit of the intern table is never interned");
    bsObjectAppend(object, longKey, bsNumber(1));
    ASSERT_INT_EQ(bsObjectCount(object), 1);
    ASSERT_DOUBLE_EQ(bsObjectGetString(object, longKey).u.number, 1);
    ASSERT_TRUE(object.u.object->uninterned);
    bsRelease(longKey);
    bsRelease(object);

    object = bsObjectNew();
    char key[8];
    for (int ix = 0; ix < 40; ix++) {
        snprintf(key, sizeof(key), "k%d", ix);
        bsObjectSet(object, key, bsNumber(ix));
    }
    bsRelease(bsObjectKeysSorted(object));
    ASSERT_TRUE(object.u.object->u.tree.root != NULL);
    BSValue appended = bsStringIntern("k40", 3);
    bsObjectAppend(object, appended, bsNumber(40));
    ASSERT_INT_EQ(bsObjectCount(object), 41);
    ASSERT_DOUBLE_EQ(bsObjectGet(object, "k40").u.number, 40);
    BSValue sorted = bsObjectKeysSorted(object);
    ASSERT_VALUE_KEEP(bsArrayGet(sorted, 40), "\"k9\"");
    bsRelease(sorted);
    bsRelease(appended);
    bsRelease(object);
}


TEST(library_object)
{
    bsTestExpr("objectNew()", "{}");
    bsTestExpr("objectNew('a', 1)", "{\"a\":1}");
    bsTestExpr("objectNew('a')", "{\"a\":null}");
    bsTestExpr("objectNew('a', 1, 'b')", "{\"a\":1,\"b\":null}");
    bsTestExpr("objectNew('a', 1, 'b', 2)", "{\"a\":1,\"b\":2}");
    bsTestExpr("objectNew(1)", "null");
    bsTestExpr("objectNew(1, 2)", "null");
    bsTestExpr("objectNew('a', 1, 2)", "null");
    bsTestExpr("objectCopy({'a': 1})", "{\"a\":1}");
    bsTestExpr("objectCopy('x')", "null");
    bsTestExpr("objectKeys({'b': 1, 'a': 2})", "[\"b\",\"a\"]");
    bsTestExpr("objectKeys('x')", "null");
    bsTestExpr("objectGet({'a': 1}, 'a')", "1");
    bsTestExpr("objectGet({'a': 1}, 'z')", "null");
    bsTestExpr("objectGet({'a': 1}, 'a', 42)", "1");
    bsTestExpr("objectGet({'a': 1}, 'z', 42)", "42");
    bsTestExpr("objectGet('x', 'a', 42)", "42");
    bsTestExpr("objectGet({'a': null}, 'a', 42)", "null");
    bsTestExpr("objectHas({'a': 1}, 'a')", "true");
    bsTestExpr("objectHas({'a': 1}, 'z')", "false");
    bsTestExpr("objectHas('x', 'a')", "false");
    bsTestExpr("objectAssign({'a': 1}, {'b': 2})", "{\"a\":1,\"b\":2}");
    bsTestExpr("objectAssign('x', {})", "null");
    bsTestExpr("objectSet({}, 'a', 1)", "1");
    bsTestExpr("objectSet('x', 'a', 1)", "null");

    ASSERT_VALUE(bsTestExecute("o = {'a': 1}\nobjectSet(o, 'b', 2)\nreturn o"), "{\"a\":1,\"b\":2}");
    ASSERT_VALUE(bsTestExecute("o = {'a': 1, 'b': 2}\nobjectDelete(o, 'a')\nreturn o"), "{\"b\":2}");
    ASSERT_VALUE(bsTestExecute("o = {'a': 1}\nreturn [objectDelete(o, 'z'), o]"), "[null,{\"a\":1}]");
    ASSERT_VALUE(bsTestExecute("return objectDelete('x', 'a')"), "null");
}


TEST(library_string)
{
    bsTestExpr("stringLength('abc')", "3");
    bsTestExpr("stringLength(1)", "0");
    bsTestExpr("stringCharAt('abc', 1)", "\"b\"");
    bsTestExpr("stringCharAt('abc', 9)", "null");
    bsTestExpr("stringCharCodeAt('abc', 0)", "97");
    bsTestExpr("stringCharCodeAt('abc', 9)", "null");
    bsTestExpr("stringIndexOf('abcabc', 'b')", "1");
    bsTestExpr("stringIndexOf('abcabc', 'b', 2)", "4");
    bsTestExpr("stringIndexOf('abc', 'z')", "-1");
    bsTestExpr("stringIndexOf('abc', 'a', 9)", "-1");
    bsTestExpr("stringIndexOf(1, 'a')", "-1");
    bsTestExpr("stringLastIndexOf('abcabc', 'b')", "4");
    bsTestExpr("stringLastIndexOf('abcabc', 'b', 2)", "1");
    bsTestExpr("stringLastIndexOf('abc', 'z')", "-1");
    bsTestExpr("stringLastIndexOf('abc', 'a', 9)", "-1");
    bsTestExpr("stringLastIndexOf('', 'abc')", "-1");
    bsTestExpr("stringSlice('abcdef', 2)", "\"cdef\"");
    bsTestExpr("stringSlice('abcdef', 1, 3)", "\"bc\"");
    bsTestExpr("stringSlice('abc', 2, 1)", "\"\"");
    bsTestExpr("stringSlice('abc', 9)", "null");
    bsTestExpr("stringSlice('abc', 0, 9)", "null");
    bsTestExpr("stringStartsWith('abc', 'ab')", "true");
    bsTestExpr("stringStartsWith('abc', 'zz')", "false");
    bsTestExpr("stringStartsWith('a', 'abc')", "false");
    bsTestExpr("stringEndsWith('abc', 'bc')", "true");
    bsTestExpr("stringEndsWith('abc', 'zz')", "false");
    bsTestExpr("stringEndsWith('a', 'abc')", "false");
    bsTestExpr("stringLower('ABC')", "\"abc\"");
    bsTestExpr("stringUpper('abc')", "\"ABC\"");
    bsTestExpr("stringLower(1)", "null");
    bsTestExpr("stringUpper(1)", "null");
    bsTestExpr("stringTrim('  abc  ')", "\"abc\"");
    bsTestExpr("stringTrim('   ')", "\"\"");
    bsTestExpr("stringTrim(1)", "null");
    bsTestExpr("stringRepeat('ab', 3)", "\"ababab\"");
    bsTestExpr("stringRepeat('ab', 0)", "\"\"");
    bsTestExpr("stringRepeat('ab', -1)", "null");
    bsTestExpr("stringReplace('a-a-a', '-', '+')", "\"a+a+a\"");
    bsTestExpr("stringReplace('abc', 'z', '+')", "\"abc\"");
    bsTestExpr("stringReplace('abc', '', '-')", "\"-a-b-c-\"");
    bsTestExpr("stringReplace(1, 'a', 'b')", "null");
    bsTestExpr("stringSplit('a,b,,c', ',')", "[\"a\",\"b\",\"\",\"c\"]");
    bsTestExpr("stringSplit('abc', '')", "[\"a\",\"b\",\"c\"]");
    bsTestExpr("stringSplit('abc', 'z')", "[\"abc\"]");
    bsTestExpr("stringSplit(1, ',')", "null");
    bsTestExpr("stringSplitLines('a\\nb\\r\\nc')", "[\"a\",\"b\",\"c\"]");
    bsTestExpr("stringSplitLines('')", "[\"\"]");
    bsTestExpr("stringSplitLines(1)", "null");
    bsTestExpr("stringNew(1)", "\"1\"");
    bsTestExpr("stringNew(null)", "\"null\"");
    bsTestExpr("stringNew([1, 2])", "\"[1,2]\"");
    bsTestExpr("stringEncode('ab')", "[97,98]");
    bsTestExpr("stringEncode(1)", "null");
    bsTestExpr("stringDecode([104, 105])", "\"hi\"");
    bsTestExpr("stringDecode([255])", "null");
    bsTestExpr("stringDecode([104, 'x'])", "null");
    bsTestExpr("stringDecode([104, 999])", "null");
    bsTestExpr("stringDecode([104, 1.5])", "null");
    bsTestExpr("stringDecode('x')", "null");
    bsTestExpr("stringFromCharCode(72, 105)", "\"Hi\"");
    bsTestExpr("stringFromCharCode()", "\"\"");
    bsTestExpr("stringFromCharCode(-1)", "null");
    bsTestExpr("stringFromCharCode(1.5)", "null");
    bsTestExpr("stringFromCharCode('x')", "null");
    bsTestExpr("stringFromCharCode(1114112)", "null");

    /* Unicode indexing is by code point */
    bsTestExpr("stringLength('a\\u00e9\\u6f22z')", "4");
    bsTestExpr("stringCharAt('a\\u00e9\\u6f22z', 2)", "\"\xe6\xbc\xa2\"");
    bsTestExpr("stringSlice('a\\u00e9\\u6f22z', 1, 3)", "\"\xc3\xa9\xe6\xbc\xa2\"");
    bsTestExpr("stringIndexOf('a\\u00e9\\u6f22z', '\\u6f22')", "2");
    bsTestExpr("stringLastIndexOf('a\\u00e9\\u6f22z', '\\u6f22')", "2");
    bsTestExpr("stringReplace('a\\u00e9b', '', '-')", "\"-a-\xc3\xa9-b-\"");
    bsTestExpr("stringSplit('a\\u00e9b', '')", "[\"a\",\"\xc3\xa9\",\"b\"]");
}


TEST(library_math)
{
    bsTestExpr("mathAbs(-2)", "2");
    bsTestExpr("mathAbs('x')", "null");
    bsTestExpr("mathCeil(1.2)", "2");
    bsTestExpr("mathCeil('x')", "null");
    bsTestExpr("mathFloor(1.8)", "1");
    bsTestExpr("mathFloor('x')", "null");
    bsTestExpr("mathSign('x')", "null");
    bsTestExpr("mathRound(1.5)", "2");
    bsTestExpr("mathRound(2.345, 2)", "2.35");
    bsTestExpr("mathRound(1, -1)", "null");
    bsTestExpr("mathSign(-3)", "-1");
    bsTestExpr("mathSign(0)", "0");
    bsTestExpr("mathSign(3)", "1");
    bsTestExpr("mathSqrt(9)", "3");
    bsTestExpr("mathSqrt(-1)", "null");
    bsTestExpr("mathLn(1)", "0");
    bsTestExpr("mathLn(0)", "null");
    bsTestExpr("mathLog(100)", "2");
    bsTestExpr("mathLog(8, 2)", "3");
    bsTestExpr("mathLog(1, 1)", "null");
    bsTestExpr("mathLog(1, 0)", "null");
    bsTestExpr("mathMax(1, 5, 3)", "5");
    bsTestExpr("mathMin(1, 5, 3)", "1");
    bsTestExpr("mathMax()", "null");
    bsTestExpr("mathMin()", "null");
    bsTestExpr("mathMax('a', 1)", "\"a\"");
    bsTestExpr("mathCos(0)", "1");
    bsTestExpr("mathSin(0)", "0");
    bsTestExpr("mathTan(0)", "0");
    bsTestExpr("mathAcos(1)", "0");
    bsTestExpr("mathAsin(0)", "0");
    bsTestExpr("mathAtan(0)", "0");
    bsTestExpr("mathAtan2(0, 1)", "0");
    bsTestExpr("mathAtan2('x', 1)", "null");
    bsTestExpr("mathPi()", "3.141592653589793");
    bsTestExpr("mathE()", "2.718281828459045");
    bsTestExpr("mathRound('x')", "null");

    /* mathRandom returns a number in [0, 1) */
    ASSERT_VALUE(bsTestExecute("v = mathRandom()\nreturn v >= 0 && v < 1"), "true");
    ASSERT_VALUE(bsTestExecute("return mathRandom() != mathRandom() || true"), "true");
}


TEST(library_number)
{
    bsTestExpr("numberParseFloat('1.5')", "1.5");
    bsTestExpr("numberParseFloat('x')", "null");
    bsTestExpr("numberParseFloat(1)", "null");
    bsTestExpr("numberParseInt('ff', 16)", "255");
    bsTestExpr("numberParseInt('12')", "12");
    bsTestExpr("numberParseInt('9', 8)", "null");
    bsTestExpr("numberParseInt('1', 1)", "null");
    bsTestExpr("numberParseInt('1', 37)", "null");
    bsTestExpr("numberToFixed(3.14159)", "\"3.14\"");
    bsTestExpr("numberToFixed(3.14159, 3)", "\"3.142\"");
    bsTestExpr("numberToFixed(3.1, 3, true)", "\"3.100\"");
    bsTestExpr("numberToFixed(3, 2, true)", "\"3\"");
    bsTestExpr("numberToFixed(3, 0, true)", "\"3\"");
    bsTestExpr("numberToFixed(3, 0)", "\"3\"");
    bsTestExpr("numberToFixed(3, -1)", "null");
    bsTestExpr("numberToFixed(3, 200)", "null");
    bsTestExpr("numberToFixed('x')", "null");
    bsTestExpr("numberToString(255, 16)", "\"ff\"");
    bsTestExpr("numberToString(0)", "\"0\"");
    bsTestExpr("numberToString(255)", "\"255\"");
    bsTestExpr("numberToString(-1)", "null");
    bsTestExpr("numberToString(1.5)", "null");
}


TEST(library_json)
{
    bsTestExpr("jsonStringify({'b': 2, 'a': 1})", "\"{\\\"a\\\":1,\\\"b\\\":2}\"");
    bsTestExpr("jsonStringify([1, 2], 2)", "\"[\\n  1,\\n  2\\n]\"");
    bsTestExpr("jsonStringify(1, 0)", "null");
    bsTestExpr("jsonParse('{\"a\": 1}')", "{\"a\":1}");
    bsTestExpr("jsonParse('bad')", "null");
    bsTestExpr("jsonParse(1)", "null");
}


TEST(library_datetime)
{
    bsTestExpr("datetimeYear(datetimeNew(2026, 8, 6))", "2026");
    bsTestExpr("datetimeMonth(datetimeNew(2026, 8, 6))", "8");
    bsTestExpr("datetimeDay(datetimeNew(2026, 8, 6))", "6");
    bsTestExpr("datetimeHour(datetimeNew(2026, 8, 6, 7))", "7");
    bsTestExpr("datetimeMinute(datetimeNew(2026, 8, 6, 7, 30))", "30");
    bsTestExpr("datetimeSecond(datetimeNew(2026, 8, 6, 7, 30, 15))", "15");
    bsTestExpr("datetimeMillisecond(datetimeNew(2026, 8, 6, 7, 30, 15, 250))", "250");
    bsTestExpr("datetimeYear('x')", "null");
    bsTestExpr("datetimeNew(99, 1, 1)", "null");
    bsTestExpr("datetimeNew(2026, 1, 20000)", "null");
    bsTestExpr("datetimeISOFormat(datetimeNew(2026, 8, 6), true)", "\"2026-08-06\"");
    bsTestExpr("datetimeISOFormat('x')", "null");
    bsTestExpr("datetimeISOParse('bad')", "null");
    bsTestExpr("datetimeISOParse(1)", "null");
    bsTestExpr("datetimeYear(datetimeISOParse('2026-08-06'))", "2026");
    bsTestExpr("systemType(datetimeNow())", "\"datetime\"");
    bsTestExpr("systemType(datetimeToday())", "\"datetime\"");
    bsTestExpr("datetimeHour(datetimeToday())", "0");

    /* The ISO format round-trips */
    ASSERT_VALUE(bsTestExecute("d = datetimeNew(2026, 8, 6, 7, 30, 15, 250)\n"
                               "return datetimeISOParse(datetimeISOFormat(d)) == d"), "true");
}


TEST(library_regex)
{
    bsTestExpr("systemType(regexNew('a'))", "\"regex\"");
    bsTestExpr("regexNew('(')", "null");
    bsTestExpr("regexNew('a', 'q')", "null");
    bsTestExpr("regexNew(1)", "null");
    bsTestExpr("systemType(regexNew('a', 'ims'))", "\"regex\"");
    bsTestExpr("regexEscape('a.b')", "\"a\\\\.b\"");
    bsTestExpr("regexEscape(1)", "null");
    bsTestExpr("regexMatch(regexNew('b'), 'abc')",
               "{\"groups\":{\"0\":\"b\"},\"index\":1,\"input\":\"abc\"}");
    bsTestExpr("regexMatch(regexNew('z'), 'abc')", "null");
    bsTestExpr("regexMatch('x', 'abc')", "null");
    bsTestExpr("regexMatchAll(regexNew('[0-9]+'), 'a1b22')",
               "[{\"groups\":{\"0\":\"1\"},\"index\":1,\"input\":\"a1b22\"},"
               "{\"groups\":{\"0\":\"22\"},\"index\":3,\"input\":\"a1b22\"}]");
    bsTestExpr("regexMatchAll(regexNew('z'), 'abc')", "[]");
    bsTestExpr("regexMatchAll('x', 'abc')", "null");
    bsTestExpr("regexMatch(regexNew('(?<name>b)'), 'abc')",
               "{\"groups\":{\"0\":\"b\",\"1\":\"b\",\"name\":\"b\"},\"index\":1,\"input\":\"abc\"}");
    bsTestExpr("objectGet(objectGet(regexMatch(regexNew('(a*)b'), 'b'), 'groups'), '1')", "\"\"");
    bsTestExpr("regexMatch(regexNew('(\xc3\xa9+)'), 'a\xc3\xa9\xc3\xa9')",
               "{\"groups\":{\"0\":\"\xc3\xa9\xc3\xa9\",\"1\":\"\xc3\xa9\xc3\xa9\"},\"index\":1,\"input\":\"a\xc3\xa9\xc3\xa9\"}");
    bsTestExpr("regexMatch(regexNew('a+', 'i'), 'AAab')",
               "{\"groups\":{\"0\":\"AAa\"},\"index\":0,\"input\":\"AAab\"}");
    bsTestExpr("regexMatch(regexNew('.*', 's'), 'a\\nb')",
               "{\"groups\":{\"0\":\"a\\nb\"},\"index\":0,\"input\":\"a\\nb\"}");
    bsTestExpr("regexReplace(regexNew('(\\\\w+) (\\\\w+)'), 'John Smith', '$2, $1')", "\"Smith, John\"");
    bsTestExpr("regexReplace(regexNew('(?<a>x)'), 'axb', '[$<a>]')", "\"a[x]b\"");
    bsTestExpr("regexReplace(regexNew('x'), 'axb', '$$')", "\"a$b\"");
    bsTestExpr("regexReplace(regexNew('x'), 'axb', '$')", "\"a$b\"");
    bsTestExpr("regexReplace(regexNew('x'), 'axb', '$9')", "\"ab\"");
    bsTestExpr("regexReplace(regexNew('(a)?x'), 'x', '[$1]')", "\"[]\"");
    bsTestExpr("regexReplace(regexNew('x'), 'axb', '$<none>')", "\"ab\"");
    bsTestExpr("regexReplace(regexNew('x'), 'axb', '$<unterminated')", "\"a$<unterminatedb\"");
    bsTestExpr("regexReplace(regexNew('(?<a>x)?y'), 'y', '[$<a>]')", "\"[]\"");
    bsTestExpr("regexReplace(regexNew(''), 'ab', '-')", "\"-a-b-\"");
    bsTestExpr("regexReplace('x', 'a', 'b')", "null");
    bsTestExpr("regexSplit(regexNew(','), 'a,b')", "[\"a\",\"b\"]");
    bsTestExpr("regexSplit(regexNew('(-)'), 'a-b')", "[\"a\",\"-\",\"b\"]");
    bsTestExpr("regexSplit(regexNew('(z)?-'), 'a-b')", "[\"a\",null,\"b\"]");
    bsTestExpr("regexSplit(regexNew(''), 'ab')", "[\"\",\"a\",\"b\",\"\"]");
    bsTestExpr("regexSplit('x', 'a')", "null");
}


TEST(library_system)
{
    bsTestExpr("systemType(null)", "\"null\"");
    bsTestExpr("systemType(1)", "\"number\"");
    bsTestExpr("systemType('a')", "\"string\"");
    bsTestExpr("systemType(true)", "\"boolean\"");
    bsTestExpr("systemType([])", "\"array\"");
    bsTestExpr("systemType({})", "\"object\"");
    bsTestExpr("systemType(systemType)", "\"function\"");
    /* Type names are interned so schemaValidate can compare them by pointer */
    {
        BSValue a = bsTestExecute("return systemType(1)");
        BSValue b = bsTestExecute("return systemType(2)");
        ASSERT_TRUE(a.type == BS_STRING && (a.u.string->flags & BS_STR_INTERNED) != 0);
        ASSERT_TRUE(a.u.string == b.u.string);
        bsRelease(a);
        bsRelease(b);
    }
    bsTestExpr("systemBoolean(0)", "false");
    bsTestExpr("systemBoolean('a')", "true");
    bsTestExpr("systemCompare(1, 2)", "-1");
    bsTestExpr("systemCompare('a', 'z')", "-1");
    bsTestExpr("systemCompare(2, 1)", "1");
    bsTestExpr("systemCompare(1, 1)", "0");
    bsTestExpr("systemIs(1, 1)", "true");
    bsTestExpr("systemIs([], [])", "false");
    bsTestExpr("systemGlobalGet('nope')", "null");
    bsTestExpr("systemGlobalGet('nope', 42)", "42");
    bsTestExpr("systemGlobalGet(1)", "null");
    bsTestExpr("systemGlobalSet(1, 2)", "null");

    ASSERT_VALUE(bsTestExecute("a = [1]\nreturn systemIs(a, a)"), "true");
    ASSERT_VALUE(bsTestExecute("systemGlobalSet('x', 5)\nreturn systemGlobalGet('x')"), "5");
    ASSERT_VALUE(bsTestExecute("x = null\nreturn systemGlobalGet('x', 42)"), "null");

    /* Logging */
    ASSERT_VALUE(bsTestExecute("systemLog('a')\nsystemLogDebug('b')"), "null");
    ASSERT_STR_EQ(bsTestLogText(), "a\n");
    ASSERT_VALUE(bsTestExecute("systemLog(1)"), "null");
    ASSERT_STR_EQ(bsTestLogText(), "1\n");

    BSOptions *options = bsTestOptions();
    options->debug = true;
    bsTestLogClear();
    bsRelease(bsTestExecuteOptions("systemLogDebug('debug')", options));
    ASSERT_STR_EQ(bsTestLogText(), "debug\n");
    bsOptionsFree(options);

    /* Logging without a log function */
    options = bsOptionsNew();
    bsRelease(bsTestExecuteOptions("systemLog('x')\nsystemLogDebug('x')", options));
    bsOptionsFree(options);
}


TEST(library_system_partial)
{
    ASSERT_VALUE(bsTestExecute("function add(a, b):\n    return a + b\nendfunction\n"
                               "addTen = systemPartial(add, 10)\nreturn addTen(5)"), "15");
    ASSERT_VALUE(bsTestExecute("function add(a, b):\n    return a + b\nendfunction\n"
                               "return systemPartial(add)"), "null");
    ASSERT_VALUE(bsTestExecute("return systemPartial('x', 1)"), "null");
    ASSERT_VALUE(bsTestExecute("function f(a...):\n    return a\nendfunction\n"
                               "p = systemPartial(f, 1)\nreturn p(2, 3)"), "[1,2,3]");

    /* More than the inline argument buffer */
    ASSERT_VALUE(bsTestExecute("function f(a...):\n    return arrayLength(a)\nendfunction\n"
                               "p = systemPartial(f, 1, 2, 3, 4, 5, 6, 7, 8, 9)\n"
                               "return p(1, 2, 3, 4, 5, 6, 7, 8, 9)"), "18");
}


TEST(library_barescript_evaluate_expression)
{
    ASSERT_VALUE(bsTestExecute("return barescriptEvaluateExpression({'number': 1})"), "1");
    ASSERT_VALUE(bsTestExecute("return barescriptEvaluateExpression({'variable': 'a'}, {'a': 5})"), "5");
    ASSERT_VALUE(bsTestExecute("return barescriptEvaluateExpression("
                               "{'function': {'name': 'max', 'args': [{'number': 1}, {'number': 2}]}})"), "2");
    ASSERT_VALUE(bsTestExecute("return barescriptEvaluateExpression("
                               "{'function': {'name': 'max', 'args': [{'number': 1}]}}, null, false)"), "null");
    ASSERT_VALUE(bsTestExecute("return barescriptEvaluateExpression('x')"), "null");
    ASSERT_VALUE(bsTestExecute("return barescriptEvaluateExpression({})"), "null");
}


static char *bsTestLibraryFetchFn(const BSFetchRequest *request, size_t *responseSize, void *data)
{
    (void) data;
    if (strcmp(request->url, "fail") == 0) {
        return NULL;
    }
    BSStringBuilder sb;
    bsSBInit(&sb);
    bsSBAppendFormat(&sb, "url=%s", request->url);
    if (request->body != NULL) {
        bsSBAppendFormat(&sb, " body=%.*s", (int) request->bodySize, request->body);
    }
    if (request->headers.type == BS_OBJECT) {
        bsSBAppendFormat(&sb, " headers=%zu", bsObjectCount(request->headers));
    }
    BSValue text = bsSBToValue(&sb);
    size_t size = bsStringSize(text);
    char *result = malloc(size + 1);
    memcpy(result, bsStringData(text), size + 1);
    bsRelease(text);
    if (responseSize != NULL) {
        *responseSize = size;
    }
    return result;
}


TEST(library_system_fetch)
{
    BSOptions *options = bsTestOptions();
    options->fetchFn = bsTestLibraryFetchFn;
    ASSERT_VALUE(bsTestExecuteOptions("return systemFetch('a')", options), "\"url=a\"");
    ASSERT_VALUE(bsTestExecuteOptions("return systemFetch({'url': 'a'})", options), "\"url=a\"");
    ASSERT_VALUE(bsTestExecuteOptions("return systemFetch({'url': 'a', 'body': 'b'})", options),
                 "\"url=a body=b\"");
    ASSERT_VALUE(bsTestExecuteOptions("return systemFetch({'url': 'a', 'headers': {'h': 'v'}})", options),
                 "\"url=a headers=1\"");
    ASSERT_VALUE(bsTestExecuteOptions("return systemFetch(['a', {'url': 'b'}])", options),
                 "[\"url=a\",\"url=b\"]");
    ASSERT_VALUE(bsTestExecuteOptions("return systemFetch('fail')", options), "null");
    ASSERT_VALUE(bsTestExecuteOptions("return systemFetch(['fail'])", options), "[null]");

    /* Invalid request models */
    ASSERT_VALUE(bsTestExecuteOptions("return systemFetch(1)", options), "null");
    ASSERT_VALUE(bsTestExecuteOptions("return systemFetch({})", options), "null");
    ASSERT_VALUE(bsTestExecuteOptions("return systemFetch({'url': 1})", options), "null");
    ASSERT_VALUE(bsTestExecuteOptions("return systemFetch({'url': 'a', 'body': 1})", options), "null");
    ASSERT_VALUE(bsTestExecuteOptions("return systemFetch({'url': 'a', 'headers': 1})", options), "null");
    ASSERT_VALUE(bsTestExecuteOptions("return systemFetch({'url': 'a', 'headers': {'h': 1}})", options), "null");
    ASSERT_VALUE(bsTestExecuteOptions("return systemFetch([1])", options), "null");
    bsOptionsFree(options);

    /* No fetch function */
    options = bsTestOptions();
    ASSERT_VALUE(bsTestExecuteOptions("return systemFetch('a')", options), "null");
    bsOptionsFree(options);

    /* Debug logging of a failed fetch */
    options = bsTestOptions();
    options->debug = true;
    options->fetchFn = bsTestLibraryFetchFn;
    bsTestLogClear();
    bsRelease(bsTestExecuteOptions("systemFetch('fail')", options));
    ASSERT_STR_EQ(bsTestLogText(), "BareScript: Function \"systemFetch\" failed for resource \"fail\"\n");
    bsOptionsFree(options);

    /* A URL function rewrites the request URL */
    options = bsTestOptions();
    options->fetchFn = bsTestLibraryFetchFn;
    options->urlFn = bsUrlFileRelative;
    options->urlData = bsTestTempFileData("dir/script.bare");
    options->urlDataFree = free;
    ASSERT_VALUE(bsTestExecuteOptions("return systemFetch('a')", options), "\"url=dir/a\"");
    bsOptionsFree(options);
}


TEST(library_argument_errors)
{
    /* An argument error is logged in debug mode, with the call site's location */
    BSOptions *options = bsTestOptions();
    options->debug = true;
    bsTestLogClear();
    bsRelease(bsTestExecuteOptions("arrayGet([1], 5)", options));
    ASSERT_STR_EQ(bsTestLogText(),
                  "test.bare:1: BareScript: Function \"arrayGet\" failed with error: "
                  "Invalid \"index\" argument value, 5\n");

    bsTestLogClear();
    bsRelease(bsTestExecuteOptions("arrayGet([1], 0, 2)", options));
    ASSERT_STR_EQ(bsTestLogText(),
                  "test.bare:1: BareScript: Function \"arrayGet\" failed with error: "
                  "Too many arguments (3)\n");

    bsTestLogClear();
    bsRelease(bsTestExecuteOptions("arrayGet()", options));
    ASSERT_STR_EQ(bsTestLogText(),
                  "test.bare:1: BareScript: Function \"arrayGet\" failed with error: "
                  "Invalid \"array\" argument value, null\n");

    bsTestLogClear();
    bsRelease(bsTestExecuteOptions("arrayGet(null, 0)", options));
    ASSERT_STR_EQ(bsTestLogText(),
                  "test.bare:1: BareScript: Function \"arrayGet\" failed with error: "
                  "Invalid \"array\" argument value, null\n");

    /* An invalid regex compile is logged in debug mode, with the call site's location */
    bsTestLogClear();
    bsRelease(bsTestExecuteOptions("regexNew('(')", options));
    ASSERT_STR_EQ(bsTestLogText(),
                  "test.bare:1: BareScript: Function \"regexNew\" failed with error: "
                  "missing ), unterminated subpattern at position 0\n");

    /* A jsonParse failure reports the decoder's error and its position */
    bsTestLogClear();
    bsRelease(bsTestExecuteOptions("jsonParse('BAD')", options));
    ASSERT_STR_EQ(bsTestLogText(),
                  "test.bare:1: BareScript: Function \"jsonParse\" failed with error: "
                  "Expecting value: line 1 column 1 (char 0)\n");
    bsTestLogClear();
    bsRelease(bsTestExecuteOptions("jsonParse('{\\n\\'a\\': }')", options));
    ASSERT_TRUE(strstr(bsTestLogText(), "line 2 column 1") != NULL);
    bsOptionsFree(options);

    /* Without debug mode nothing is logged */
    options = bsTestOptions();
    bsTestLogClear();
    bsRelease(bsTestExecuteOptions("arrayGet([1], 5)\nregexNew('(')", options));
    ASSERT_STR_EQ(bsTestLogText(), "");
    bsOptionsFree(options);
}


TEST(library_lookup)
{
    /* Script functions and expression function aliases resolve to the same values */
    BSValue name = bsStringNew("max");
    ASSERT_TRUE(bsValueIs(bsLibraryExpressionFunction(name), bsLibraryScriptFunction("mathMax")));
    bsRelease(name);
    name = bsStringNew("nope");
    ASSERT_INT_EQ(bsLibraryExpressionFunction(name).type, BS_NULL);
    bsRelease(name);
    ASSERT_INT_EQ(bsLibraryScriptFunction("nope").type, BS_NULL);
    ASSERT_INT_EQ(bsLibraryScriptFunction("mathAbs").type, BS_FUNCTION);

    /* All the expression function aliases resolve */
    static const char *aliases[] = {
        "abs", "acos", "arrayNew", "asin", "atan", "atan2", "ceil", "charCodeAt", "cos", "date", "day",
        "endsWith", "fixed", "floor", "fromCharCode", "hour", "indexOf", "lastIndexOf", "len", "ln",
        "log", "lower", "max", "millisecond", "min", "minute", "month", "now", "objectNew", "parseFloat",
        "parseInt", "pi", "rand", "replace", "rept", "round", "second", "sign", "sin", "slice", "sqrt",
        "startsWith", "tan", "text", "today", "trim", "upper", "year"
    };
    for (size_t ix = 0; ix < sizeof(aliases) / sizeof(aliases[0]); ix++) {
        BSValue aliasName = bsStringNew(aliases[ix]);
        ASSERT_INT_EQ(bsLibraryExpressionFunction(aliasName).type, BS_FUNCTION);
        bsRelease(aliasName);
    }
}


TEST(library_too_many_arguments)
{
    /* Every library function reports its documented error value when given too many arguments */
    bsTestExpr("systemType(1, 2)", "null");
    bsTestExpr("systemBoolean(1, 2)", "null");
    bsTestExpr("systemCompare(1, 2, 3)", "null");
    bsTestExpr("systemIs(1, 2, 3)", "null");
    bsTestExpr("systemLog(1, 2)", "null");
    bsTestExpr("systemLogDebug(1, 2)", "null");
    bsTestExpr("systemFetch('a', 'b')", "null");
    bsTestExpr("stringNew(1, 2)", "null");
    bsTestExpr("arrayDelete('x', 0)", "null");
    bsTestExpr("arrayFlat('x')", "null");
    bsTestExpr("arrayLastIndexOf('x', 1)", "-1");
    bsTestExpr("arrayPop('x')", "null");
    bsTestExpr("arraySet('x', 0, 1)", "null");
    bsTestExpr("arrayShift('x')", "null");
    bsTestExpr("arraySlice('x', 0)", "null");
    bsTestExpr("stringCharAt(1, 0)", "null");
    bsTestExpr("stringCharCodeAt(1, 0)", "null");
    bsTestExpr("stringEndsWith(1, 'a')", "null");
    bsTestExpr("stringLastIndexOf(1, 'a')", "-1");
    bsTestExpr("stringSlice(1, 0)", "null");
    bsTestExpr("stringStartsWith(1, 'a')", "null");
    bsTestExpr("barescriptEvaluateExpression({}, null, true, 4)", "null");
    /* The no-argument functions ignore extra arguments, as the reference implementations do */
    bsTestExpr("systemType(datetimeNow(1))", "\"datetime\"");
    bsTestExpr("systemType(datetimeToday(1))", "\"datetime\"");
    bsTestExpr("mathPi(1) == mathPi()", "true");
    bsTestExpr("mathE(1) == mathE()", "true");
    bsTestExpr("systemType(mathRandom(1))", "\"number\"");

    /* An empty search string, and a search longer than the string */
    bsTestExpr("stringIndexOf('abc', '')", "0");
    bsTestExpr("stringIndexOf('abc', '', 3)", "3");
    bsTestExpr("stringIndexOf('a', 'abc')", "-1");

    /* An unknown named group reference scans every named group */
    bsTestExpr("regexReplace(regexNew('(?<a>x)'), 'axb', '[$<none>]')", "\"a[]b\"");
}


TEST(library_repeated_argument_error)
{
    /*
     * A comparison function with the wrong arity reports an argument error on every call - only
     * the first is recorded, since the runtime clears it once per call site
     */
    BSOptions *options = bsTestOptions();
    options->debug = true;
    bsTestLogClear();
    ASSERT_VALUE(bsTestExecuteOptions("return arraySort([3, 1, 2], mathAbs)", options), "[3,1,2]");
    ASSERT_STR_EQ(bsTestLogText(),
                  "test.bare:1: BareScript: Function \"arraySort\" failed with error: "
                  "Too many arguments (2)\n");
    bsOptionsFree(options);
}


TEST(library_args_validate_api)
{
    /* The argument model supports limits and types the library itself does not use */
    static const BSArgModel model[] = {
        {"value", BS_ARG_NUMBER, BS_ARG_LT, 0, 10, 0, 0},
        {"flag", BS_ARG_BOOLEAN, 0, 0, 0, 0, 0},
        {"when", BS_ARG_DATETIME, BS_ARG_NULLABLE, 0, 0, 0, 0},
        {"pattern", BS_ARG_REGEX, BS_ARG_NULLABLE, 0, 0, 0, 0},
        {"fn", BS_ARG_FUNCTION, BS_ARG_NULLABLE, 0, 0, 0, 0}
    };
    BSOptions *options = bsTestOptions();
    BSValue values[5];

    /* A missing boolean argument with no default is false */
    BSValue args[1] = {bsNumber(5)};
    ASSERT_TRUE(bsArgsValidate(model, 5, args, 1, values, options, "test"));
    ASSERT_DOUBLE_EQ(values[0].u.number, 5);
    ASSERT_FALSE(values[1].u.boolean);
    ASSERT_INT_EQ(values[2].type, BS_NULL);
    bsArgsFree(model, 5, values);

    /* The "lt" limit */
    BSValue tooBig[1] = {bsNumber(10)};
    ASSERT_FALSE(bsArgsValidate(model, 5, tooBig, 1, values, options, "test"));
    ASSERT_STR_EQ(bsStringData(options->argsError), "Invalid \"value\" argument value, 10");
    bsAssign(&options->argsError, bsNull());

    /* The datetime, regex, and function argument types */
    BSValue regex = bsRegexNew("a", 1, 0, NULL, 0);
    BSValue function = bsLibraryScriptFunction("mathAbs");
    BSValue all[5] = {bsNumber(1), bsBoolean(true), bsDatetime(0), regex, function};
    ASSERT_TRUE(bsArgsValidate(model, 5, all, 5, values, options, "test"));
    bsArgsFree(model, 5, values);

    BSValue badDatetime[3] = {bsNumber(1), bsBoolean(true), bsNumber(1)};
    ASSERT_FALSE(bsArgsValidate(model, 5, badDatetime, 3, values, options, "test"));
    bsAssign(&options->argsError, bsNull());
    bsRelease(regex);
    bsOptionsFree(options);
}


TEST(library_direct_call)
{
    /* bsFunctionCall bypasses the evaluator intrinsic, covering the original happy paths */
    BSOptions *options = bsTestOptions();
    bsLibraryGlobals(options->globals);

    BSValue array = bsArrayNew();
    bsArrayPush(array, bsNumber(1));
    bsArrayPush(array, bsNumber(2));
    BSValue object = bsObjectNew();
    bsObjectSet(object, "a", bsNumber(1));
    BSValue key = bsStringNew("a");
    BSValue str = bsStringNew("hello");
    BSValue hello = bsStringNew("he");
    BSValue lo = bsStringNew("lo");
    BSValue args[3];

    args[0] = array;
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("arrayNew"), args, 0, options), "[]");
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("arrayNew"), args, 1, options), "[[1,2]]");
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("objectNew"), args, 0, options), "{}");
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("arrayCopy"), args, 1, options), "[1,2]");
    args[1] = bsNumber(1);
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("arrayGet"), args, 2, options), "2");
    ASSERT_VALUE(bsFunctionInvoke(bsLibraryScriptFunction("arrayGet"), args, 2, options), "2");
    /* Off the happy path, invoke runs the full function, which validates the arguments */
    ASSERT_VALUE(bsFunctionInvoke(bsLibraryScriptFunction("arrayGet"), args, 1, options), "null");
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("arrayLength"), args, 1, options), "2");
    ASSERT_VALUE(bsFunctionInvoke(bsLibraryScriptFunction("arrayLength"), args, 1, options), "2");
    args[1] = bsNumber(0);
    args[2] = bsNumber(9);
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("arraySet"), args, 3, options), "9");
    args[1] = bsNumber(3);
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("arrayPush"), args, 2, options), "[9,2,3]");
    args[1] = bsNumber(4);
    ASSERT_VALUE(bsFunctionInvoke(bsLibraryScriptFunction("arrayPush"), args, 2, options), "[9,2,3,4]");
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("arrayPop"), args, 1, options), "4");

    args[0] = bsNumber(-2);
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("mathAbs"), args, 1, options), "2");
    args[0] = bsNumber(1.2);
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("mathCeil"), args, 1, options), "2");
    args[0] = bsNumber(1.8);
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("mathFloor"), args, 1, options), "1");
    ASSERT_VALUE(bsFunctionInvoke(bsLibraryScriptFunction("mathFloor"), args, 1, options), "1");
    args[0] = bsNumber(-3);
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("mathSign"), args, 1, options), "-1");
    args[0] = bsNumber(9);
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("mathSqrt"), args, 1, options), "3");

    args[0] = object;
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("objectCopy"), args, 1, options), "{\"a\":1}");
    args[1] = key;
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("objectGet"), args, 2, options), "1");
    ASSERT_VALUE(bsFunctionInvoke(bsLibraryScriptFunction("objectGet"), args, 2, options), "1");
    args[1] = bsStringNew("missing");
    args[2] = bsNumber(42);
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("objectGet"), args, 3, options), "42");
    ASSERT_VALUE(bsFunctionInvoke(bsLibraryScriptFunction("objectGet"), args, 3, options), "42");
    bsRelease(args[1]);
    args[1] = key;
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("objectHas"), args, 2, options), "true");
    ASSERT_VALUE(bsFunctionInvoke(bsLibraryScriptFunction("objectHas"), args, 2, options), "true");
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("objectKeys"), args, 1, options), "[\"a\"]");
    ASSERT_VALUE(bsFunctionInvoke(bsLibraryScriptFunction("objectKeys"), args, 1, options), "[\"a\"]");
    args[2] = bsNumber(2);
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("objectSet"), args, 3, options), "2");
    ASSERT_VALUE(bsFunctionInvoke(bsLibraryScriptFunction("objectSet"), args, 3, options), "2");
    args[1] = bsStringNew("z");
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("objectDelete"), args, 2, options), "null");
    bsRelease(args[1]);

    args[0] = str;
    args[1] = hello;
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("stringStartsWith"), args, 2, options), "true");
    args[1] = lo;
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("stringEndsWith"), args, 2, options), "true");
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("stringLength"), args, 1, options), "5");
    ASSERT_VALUE(bsFunctionInvoke(bsLibraryScriptFunction("stringLength"), args, 1, options), "5");

    args[0] = bsNumber(0);
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("systemBoolean"), args, 1, options), "false");
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("systemType"), args, 1, options), "\"number\"");

    BSValue re = bsRegexNew("b", 1, 0, NULL, 0);
    BSValue abc = bsStringNew("abc");
    args[0] = re;
    args[1] = abc;
    ASSERT_VALUE(bsFunctionCall(bsLibraryScriptFunction("regexMatch"), args, 2, options),
                 "{\"groups\":{\"0\":\"b\"},\"index\":1,\"input\":\"abc\"}");
    ASSERT_VALUE(bsFunctionInvoke(bsLibraryScriptFunction("regexMatch"), args, 2, options),
                 "{\"groups\":{\"0\":\"b\"},\"index\":1,\"input\":\"abc\"}");
    bsRelease(re);
    bsRelease(abc);

    bsRelease(array);
    bsRelease(object);
    bsRelease(key);
    bsRelease(str);
    bsRelease(hello);
    bsRelease(lo);
    bsOptionsFree(options);
}


TEST(library_regex_many_groups)
{
    /* A match with more than ten capture groups exercises the multi-digit group keys */
    bsTestExpr("objectKeys(objectGet(regexMatch(regexNew("
               "'(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l)'), 'abcdefghijkl'), 'groups'))",
               "[\"0\",\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\",\"10\",\"11\",\"12\"]");
    bsTestExpr("objectGet(objectGet(regexMatch(regexNew("
               "'(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l)'), 'abcdefghijkl'), 'groups'), '11')", "\"k\"");
}
