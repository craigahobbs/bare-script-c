/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The value system unit tests
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "test.h"


TEST(value_null)
{
    BSValue value = bsNull();
    ASSERT_INT_EQ(value.type, BS_NULL);
    ASSERT_STR_EQ(bsValueTypeString(value), "null");
    ASSERT_VALUE_STRING(bsNull(), "null");
    ASSERT_VALUE(bsNull(), "null");
    ASSERT_FALSE(bsValueBoolean(bsNull()));
}


TEST(value_boolean)
{
    ASSERT_STR_EQ(bsValueTypeString(bsBoolean(true)), "boolean");
    ASSERT_VALUE_STRING(bsBoolean(true), "true");
    ASSERT_VALUE_STRING(bsBoolean(false), "false");
    ASSERT_VALUE(bsBoolean(true), "true");
    ASSERT_TRUE(bsValueBoolean(bsBoolean(true)));
    ASSERT_FALSE(bsValueBoolean(bsBoolean(false)));
}


TEST(value_number)
{
    ASSERT_STR_EQ(bsValueTypeString(bsNumber(1)), "number");
    ASSERT_VALUE_STRING(bsNumber(0), "0");
    ASSERT_VALUE_STRING(bsNumber(-0.0), "0");
    ASSERT_VALUE_STRING(bsNumber(1), "1");
    ASSERT_VALUE_STRING(bsNumber(-1.5), "-1.5");
    ASSERT_VALUE_STRING(bsNumber(1e21), "1e+21");
    ASSERT_VALUE_STRING(bsNumber(1e-7), "1e-7");
    ASSERT_VALUE_STRING(bsNumber(1.0 / 3.0), "0.3333333333333333");
    ASSERT_VALUE_STRING(bsNumber(0.1 + 0.2), "0.30000000000000004");
    ASSERT_VALUE_STRING(bsNumber(1e20), "100000000000000000000");
    ASSERT_VALUE_STRING(bsNumber(0.000001), "0.000001");
    ASSERT_VALUE_STRING(bsNumber(1e100), "1e+100");
    ASSERT_VALUE_STRING(bsNumber(1.25e-10), "1.25e-10");
    ASSERT_TRUE(bsValueBoolean(bsNumber(1)));
    ASSERT_FALSE(bsValueBoolean(bsNumber(0)));
}


TEST(value_number_format_special)
{
    char buffer[64];
    bsNumberFormat(NAN, buffer, sizeof(buffer));
    ASSERT_STR_EQ(buffer, "NaN");
    bsNumberFormat(INFINITY, buffer, sizeof(buffer));
    ASSERT_STR_EQ(buffer, "Infinity");
    bsNumberFormat(-INFINITY, buffer, sizeof(buffer));
    ASSERT_STR_EQ(buffer, "-Infinity");
}


TEST(value_number_round)
{
    ASSERT_DOUBLE_EQ(bsNumberRound(1.5, 0), 2);
    ASSERT_DOUBLE_EQ(bsNumberRound(-1.5, 0), -2);
    ASSERT_DOUBLE_EQ(bsNumberRound(2.345, 2), 2.35);
    ASSERT_DOUBLE_EQ(bsNumberRound(1.0, 0), 1);
}


TEST(value_number_parse)
{
    double number = 0;
    ASSERT_TRUE(bsNumberParse("1.5", 3, &number));
    ASSERT_DOUBLE_EQ(number, 1.5);
    ASSERT_TRUE(bsNumberParse("  -2e3  ", 8, &number));
    ASSERT_DOUBLE_EQ(number, -2000);
    ASSERT_TRUE(bsNumberParse("+.5", 3, &number));
    ASSERT_DOUBLE_EQ(number, 0.5);
    ASSERT_TRUE(bsNumberParse("5.", 2, &number));
    ASSERT_DOUBLE_EQ(number, 5);
    ASSERT_FALSE(bsNumberParse("", 0, &number));
    ASSERT_FALSE(bsNumberParse("x", 1, &number));
    ASSERT_FALSE(bsNumberParse("1x", 2, &number));
    ASSERT_FALSE(bsNumberParse("1e", 2, &number));
    ASSERT_FALSE(bsNumberParse("1e+", 3, &number));
    ASSERT_FALSE(bsNumberParse("1e999", 5, &number));
    ASSERT_FALSE(bsNumberParse(".", 1, &number));
    char big[80];
    memset(big, '1', sizeof(big));
    ASSERT_FALSE(bsNumberParse(big, sizeof(big), &number));
}


TEST(value_integer_parse)
{
    double number = 0;
    ASSERT_TRUE(bsIntegerParse("ff", 2, 16, &number));
    ASSERT_DOUBLE_EQ(number, 255);
    ASSERT_TRUE(bsIntegerParse("  -101  ", 8, 2, &number));
    ASSERT_DOUBLE_EQ(number, -5);
    ASSERT_TRUE(bsIntegerParse("+7", 2, 10, &number));
    ASSERT_DOUBLE_EQ(number, 7);
    ASSERT_TRUE(bsIntegerParse("Z", 1, 36, &number));
    ASSERT_DOUBLE_EQ(number, 35);
    ASSERT_FALSE(bsIntegerParse("9", 1, 8, &number));
    ASSERT_FALSE(bsIntegerParse("", 0, 10, &number));
    ASSERT_FALSE(bsIntegerParse("1x", 2, 10, &number));
    ASSERT_FALSE(bsIntegerParse("1", 1, 1, &number));
    ASSERT_FALSE(bsIntegerParse("1", 1, 37, &number));
    char big[400];
    memset(big, '9', sizeof(big));
    ASSERT_FALSE(bsIntegerParse(big, sizeof(big), 10, &number));
}


TEST(value_string)
{
    BSValue value = bsStringNew("abc");
    ASSERT_STR_EQ(bsValueTypeString(value), "string");
    ASSERT_STR_EQ(bsStringData(value), "abc");
    ASSERT_INT_EQ(bsStringSize(value), 3);
    ASSERT_INT_EQ(bsStringLength(value), 3);
    ASSERT_TRUE(bsValueBoolean(value));
    bsRelease(value);

    BSValue empty = bsStringNew("");
    ASSERT_FALSE(bsValueBoolean(empty));
    bsRelease(empty);

    ASSERT_VALUE_STRING(bsStringNewSize("abcdef", 3), "abc");
    ASSERT_VALUE_STRING(bsStringNewFormat("%s-%d", "x", 5), "x-5");
    /* Word-at-a-time ASCII length for strings of 8 bytes or more */
    BSValue ascii = bsStringNew("abcdefghijkl");
    ASSERT_INT_EQ(bsStringLength(ascii), 12);
    bsRelease(ascii);

    /* A non-string value has no string data */
    ASSERT_STR_EQ(bsStringData(bsNumber(1)), "");
    ASSERT_INT_EQ(bsStringSize(bsNumber(1)), 0);
    ASSERT_INT_EQ(bsStringLength(bsNumber(1)), 0);
}


TEST(value_string_unicode)
{
    BSValue value = bsStringNew("a\xc3\xa9\xe6\xbc\xa2\xf0\x9f\x98\x80z");
    ASSERT_INT_EQ(bsStringLength(value), 5);
    ASSERT_INT_EQ(bsStringSize(value), 11);
    ASSERT_INT_EQ(bsStringOffset(value, 0), 0);
    ASSERT_INT_EQ(bsStringOffset(value, 1), 1);
    ASSERT_INT_EQ(bsStringOffset(value, 2), 3);
    ASSERT_INT_EQ(bsStringOffset(value, 3), 6);
    ASSERT_INT_EQ(bsStringOffset(value, 5), 11);
    ASSERT_INT_EQ(bsStringOffset(value, 99), 11);
    ASSERT_INT_EQ(bsStringOffset(value, 4), 10);
    ASSERT_INT_EQ(bsStringOffset(value, 1), 1);
    ASSERT_INT_EQ(bsStringOffset(value, 1), 1);
    ASSERT_INT_EQ(bsStringCodePoint(value, 0), 'a');
    ASSERT_INT_EQ(bsStringCodePoint(value, 1), 0xE9);
    ASSERT_INT_EQ(bsStringCodePoint(value, 3), 0x1F600);
    ASSERT_INT_EQ(bsStringCodePoint(value, 99), 0);
    bsRelease(value);

    /* An ASCII string indexes by byte */
    BSValue ascii = bsStringNew("abc");
    ASSERT_INT_EQ(bsStringOffset(ascii, 1), 1);
    ASSERT_INT_EQ(bsStringOffset(ascii, 9), 3);
    bsRelease(ascii);

    /* A non-string value */
    ASSERT_INT_EQ(bsStringOffset(bsNumber(1), 0), 0);

    /* Format a non-ASCII string so length walks UTF-8 */
    BSValue formatted = bsStringNewFormat("%s", "\xc3\xa9");
    ASSERT_INT_EQ(bsStringLength(formatted), 1);
    ASSERT_INT_EQ(bsStringSize(formatted), 2);
    bsRelease(formatted);

    /* A long non-ASCII string builds a sparse index on a backward lookup */
    BSStringBuilder sb;
    bsSBInit(&sb);
    for (int ix = 0; ix < 40; ix++) {
        bsSBAppendString(&sb, "\xc3\xa9");
    }
    BSValue longUnicode = bsSBToValue(&sb);
    ASSERT_INT_EQ(bsStringOffset(longUnicode, 0), 0);
    ASSERT_INT_EQ(bsStringOffset(longUnicode, 1), 2);
    ASSERT_INT_EQ(bsStringOffset(longUnicode, 39), 78);
    ASSERT_INT_EQ(bsStringOffset(longUnicode, 40), 80);
    ASSERT_INT_EQ(bsStringOffset(longUnicode, 99), 80);
    ASSERT_INT_EQ(bsStringOffset(longUnicode, 1), 2);
    ASSERT_INT_EQ(bsStringOffset(longUnicode, 20), 40);
    ASSERT_INT_EQ(bsStringOffset(longUnicode, 16), 32);
    bsRelease(longUnicode);
}


TEST(value_utf8)
{
    char buffer[4];
    ASSERT_INT_EQ(bsUTF8Encode('a', buffer), 1);
    ASSERT_INT_EQ(bsUTF8Encode(0xE9, buffer), 2);
    ASSERT_INT_EQ(bsUTF8Encode(0x6F22, buffer), 3);
    ASSERT_INT_EQ(bsUTF8Encode(0x1F600, buffer), 4);

    size_t codeSize = 0;
    ASSERT_INT_EQ(bsUTF8Decode("a", 1, 0, &codeSize), 'a');
    ASSERT_INT_EQ(codeSize, 1);
    ASSERT_INT_EQ(bsUTF8Decode("\xc3\xa9", 2, 0, &codeSize), 0xE9);
    ASSERT_INT_EQ(codeSize, 2);
    ASSERT_INT_EQ(bsUTF8Decode("\xe6\xbc\xa2", 3, 0, &codeSize), 0x6F22);
    ASSERT_INT_EQ(codeSize, 3);
    ASSERT_INT_EQ(bsUTF8Decode("\xf0\x9f\x98\x80", 4, 0, &codeSize), 0x1F600);
    ASSERT_INT_EQ(codeSize, 4);

    /* Invalid sequences decode as the replacement character */
    ASSERT_INT_EQ(bsUTF8Decode("\xff", 1, 0, &codeSize), 0xFFFD);
    ASSERT_INT_EQ(bsUTF8Decode("\xc3", 1, 0, &codeSize), 0xFFFD);
    ASSERT_INT_EQ(bsUTF8Decode("\xc3\x41", 2, 0, &codeSize), 0xFFFD);

    /* Strict validation */
    ASSERT_INT_EQ(bsUTF8Length("abc", 3), 3);
    ASSERT_INT_EQ(bsUTF8Length("\xc3\xa9", 2), 1);
    ASSERT_TRUE(bsUTF8Length("\xff", 1) == SIZE_MAX);
    ASSERT_TRUE(bsUTF8Length("\xc0\x80", 2) == SIZE_MAX);
    ASSERT_TRUE(bsUTF8Length("\xe0\x80\x80", 3) == SIZE_MAX);
    ASSERT_TRUE(bsUTF8Length("\xed\xa0\x80", 3) == SIZE_MAX);
    ASSERT_TRUE(bsUTF8Length("\xf0\x80\x80\x80", 4) == SIZE_MAX);
    ASSERT_TRUE(bsUTF8Length("\xf4\x90\x80\x80", 4) == SIZE_MAX);
    ASSERT_TRUE(bsUTF8Length("\xf5\x80\x80\x80", 4) == SIZE_MAX);
    ASSERT_TRUE(bsUTF8Length("\xc3", 1) == SIZE_MAX);
    ASSERT_TRUE(bsUTF8Length("\xc3\x41", 2) == SIZE_MAX);
    ASSERT_INT_EQ(bsUTF8Length("\xe1\x80\x80", 3), 1);
    ASSERT_INT_EQ(bsUTF8Length("\xf1\x80\x80\x80", 4), 1);
    ASSERT_INT_EQ(bsUTF8Length("\xed\x80\x80", 3), 1);
    ASSERT_INT_EQ(bsUTF8Length("\xf4\x80\x80\x80", 4), 1);

    /* An invalid UTF-8 string falls back to byte length */
    BSValue value = bsStringNewSize("\xff\xfe", 2);
    ASSERT_INT_EQ(bsStringLength(value), 2);
    bsRelease(value);
}


TEST(value_string_builder)
{
    BSStringBuilder sb;
    bsSBInit(&sb);
    bsSBAppendString(&sb, "abc");
    bsSBAppendChar(&sb, '-');
    bsSBAppend(&sb, "defgh", 3);
    bsSBAppend(&sb, "", 0);
    bsSBAppendFormat(&sb, "[%d]", 42);
    bsSBAppendValue(&sb, bsNumber(7));
    ASSERT_VALUE_STRING(bsSBToValue(&sb), "abc-def[42]7");

    /* An empty builder */
    bsSBInit(&sb);
    ASSERT_VALUE_STRING(bsSBToValue(&sb), "");

    /* Growth past the initial capacity */
    bsSBInit(&sb);
    for (int ix = 0; ix < 200; ix++) {
        bsSBAppendChar(&sb, 'x');
    }
    BSValue value = bsSBToValue(&sb);
    ASSERT_INT_EQ(bsStringSize(value), 200);
    bsRelease(value);

    /* Free without converting */
    bsSBInit(&sb);
    bsSBAppendString(&sb, "abc");
    bsSBFree(&sb);
    ASSERT_NULL(sb.data);
}


TEST(value_array)
{
    BSValue array = bsArrayNew();
    ASSERT_STR_EQ(bsValueTypeString(array), "array");
    ASSERT_INT_EQ(bsArrayCount(array), 0);
    ASSERT_FALSE(bsValueBoolean(array));

    bsArrayPush(array, bsNumber(1));
    bsArrayPush(array, bsStringNew("two"));
    ASSERT_INT_EQ(bsArrayCount(array), 2);
    ASSERT_TRUE(bsValueBoolean(array));
    ASSERT_DOUBLE_EQ(bsArrayGet(array, 0).u.number, 1);
    ASSERT_INT_EQ(bsArrayGet(array, 9).type, BS_NULL);
    ASSERT_INT_EQ(bsArrayCount(bsNumber(1)), 0);
    ASSERT_INT_EQ(bsArrayGet(bsNumber(1), 0).type, BS_NULL);

    bsArraySet(array, 0, bsNumber(9));
    ASSERT_DOUBLE_EQ(bsArrayGet(array, 0).u.number, 9);

    bsArrayInsert(array, 1, bsStringNew("mid"));
    ASSERT_VALUE_KEEP(array, "[9,\"mid\",\"two\"]");

    bsArrayDelete(array, 1);
    ASSERT_VALUE_KEEP(array, "[9,\"two\"]");

    BSValue copy = bsArrayCopy(array);
    ASSERT_VALUE(copy, "[9,\"two\"]");
    ASSERT_VALUE(bsArrayCopy(bsNumber(1)), "[]");

    bsArrayReserve(array, 100);
    ASSERT_INT_EQ(bsArrayCount(array), 2);
    bsRelease(array);
}


static int bsTestCompareValues(BSValue value1, BSValue value2, void *data)
{
    (void) data;
    return bsValueCompare(value1, value2);
}


TEST(value_array_sort)
{
    BSValue array = bsArrayNew();
    bsArraySort(array, bsTestCompareValues, NULL);
    ASSERT_VALUE_KEEP(array, "[]");

    bsArrayPush(array, bsNumber(3));
    bsArraySort(array, bsTestCompareValues, NULL);
    ASSERT_VALUE_KEEP(array, "[3]");

    bsArrayPush(array, bsNumber(1));
    bsArrayPush(array, bsNumber(2));
    bsArrayPush(array, bsNumber(5));
    bsArrayPush(array, bsNumber(4));
    bsArraySort(array, bsTestCompareValues, NULL);
    ASSERT_VALUE(array, "[1,2,3,4,5]");
}


TEST(value_object)
{
    BSValue object = bsObjectNew();
    ASSERT_STR_EQ(bsValueTypeString(object), "object");
    ASSERT_INT_EQ(bsObjectCount(object), 0);
    ASSERT_TRUE(bsValueBoolean(object));
    ASSERT_FALSE(bsObjectHas(object, "z"));
    ASSERT_FALSE(bsObjectHas(object, "this-key-is-longer-than-sixty-four-bytes-so-it-is-not-interned-xx"));

    bsObjectSet(object, "b", bsNumber(2));
    bsObjectSet(object, "a", bsNumber(1));
    bsObjectSet(object, "c", bsNumber(3));
    ASSERT_INT_EQ(bsObjectCount(object), 3);
    ASSERT_TRUE(bsObjectHas(object, "a"));
    ASSERT_FALSE(bsObjectHas(object, "z"));
    ASSERT_DOUBLE_EQ(bsObjectGet(object, "b").u.number, 2);
    bsObjectSet(object, "", bsNumber(0));
    ASSERT_DOUBLE_EQ(bsObjectGet(object, "").u.number, 0);
    ASSERT_TRUE(bsObjectDelete(object, ""));
    ASSERT_INT_EQ(bsObjectGet(object, "z").type, BS_NULL);

    /* Objects iterate in insertion order and serialize in sorted key order */
    ASSERT_VALUE(bsObjectKeys(object), "[\"b\",\"a\",\"c\"]");
    ASSERT_VALUE(bsObjectKeysSorted(object), "[\"a\",\"b\",\"c\"]");
    ASSERT_VALUE_KEEP(object, "{\"a\":1,\"b\":2,\"c\":3}");

    /* An update keeps the key's insertion position */
    bsObjectSet(object, "b", bsNumber(9));
    ASSERT_INT_EQ(bsObjectCount(object), 3);
    ASSERT_VALUE(bsObjectKeys(object), "[\"b\",\"a\",\"c\"]");

    BSValue copy = bsObjectCopy(object);
    ASSERT_VALUE(bsObjectKeys(copy), "[\"b\",\"a\",\"c\"]");
    bsRelease(copy);

    /* Delete from the head, middle, and tail of the insertion list */
    ASSERT_TRUE(bsObjectDelete(object, "b"));
    ASSERT_FALSE(bsObjectDelete(object, "b"));
    ASSERT_VALUE(bsObjectKeys(object), "[\"a\",\"c\"]");
    ASSERT_TRUE(bsObjectDelete(object, "c"));
    ASSERT_VALUE(bsObjectKeys(object), "[\"a\"]");
    ASSERT_TRUE(bsObjectDelete(object, "a"));
    ASSERT_INT_EQ(bsObjectCount(object), 0);
    bsRelease(object);

    /* Non-object values */
    ASSERT_INT_EQ(bsObjectCount(bsNumber(1)), 0);
    ASSERT_INT_EQ(bsObjectGet(bsNumber(1), "a").type, BS_NULL);
    ASSERT_FALSE(bsObjectHas(bsNumber(1), "a"));
    ASSERT_VALUE(bsObjectKeys(bsNumber(1)), "[]");
    ASSERT_VALUE(bsObjectCopy(bsNumber(1)), "{}");
    BSValue key = bsStringNew("a");
    ASSERT_INT_EQ(bsObjectGetString(bsNumber(1), key).type, BS_NULL);
    ASSERT_FALSE(bsObjectHasString(bsNumber(1), key));
    bsRelease(key);
}


TEST(value_object_intern)
{
    /* Enough unique short keys to grow the intern table (32 slots, grow at 75%) */
    BSValue object = bsObjectNew();
    char key[16];
    for (int ix = 0; ix < 40; ix++) {
        snprintf(key, sizeof(key), "k%d", ix);
        bsObjectSet(object, key, bsNumber(ix));
    }
    ASSERT_INT_EQ(bsObjectCount(object), 40);
    ASSERT_DOUBLE_EQ(bsObjectGet(object, "k0").u.number, 0);
    ASSERT_DOUBLE_EQ(bsObjectGet(object, "k39").u.number, 39);
    ASSERT_FALSE(bsObjectHas(object, "k40"));
    ASSERT_FALSE(bsObjectDelete(object, "k40"));
    ASSERT_TRUE(bsObjectDelete(object, "k0"));
    ASSERT_FALSE(bsObjectHas(object, "k0"));
    ASSERT_DOUBLE_EQ(bsObjectGet(object, "k39").u.number, 39);

    /* Further inserts grow the interned-pointer hash table */
    for (int ix = 40; ix < 80; ix++) {
        snprintf(key, sizeof(key), "k%d", ix);
        bsObjectSet(object, key, bsNumber(ix));
    }
    ASSERT_DOUBLE_EQ(bsObjectGet(object, "k79").u.number, 79);

    /* Interned keys from the object compare by pointer; a 4-byte key hits the word hash */
    BSValue keys = bsObjectKeys(object);
    ASSERT_TRUE(bsObjectHasString(object, bsArrayGet(keys, 0)));
    bsRelease(keys);
    bsObjectSet(object, "abcd", bsNumber(4));
    ASSERT_DOUBLE_EQ(bsObjectGet(object, "abcd").u.number, 4);
    bsObjectSet(object, "\xc3\xa9", bsNumber(5));
    ASSERT_DOUBLE_EQ(bsObjectGet(object, "\xc3\xa9").u.number, 5);

    /* A key longer than the intern limit still round-trips, on both small and large objects */
    char longKey[80];
    memset(longKey, 'a', 70);
    longKey[70] = '\0';
    BSValue small = bsObjectNew();
    bsObjectSet(small, longKey, bsNumber(1));
    ASSERT_TRUE(bsObjectHas(small, longKey));
    bsRelease(small);
    bsObjectSet(object, longKey, bsNumber(70));
    ASSERT_TRUE(bsObjectHas(object, longKey));
    ASSERT_DOUBLE_EQ(bsObjectGet(object, longKey).u.number, 70);
    ASSERT_TRUE(bsObjectDelete(object, longKey));
    ASSERT_FALSE(bsObjectHas(object, longKey));
    bsRelease(object);
}


TEST(value_object_small_update)
{
    /* A long (non-interned) key updated on a small list-only object */
    BSValue object = bsObjectNew();
    char longKey[80];
    memset(longKey, 'b', 70);
    longKey[70] = '\0';
    BSValue key = bsStringNew(longKey);
    bsObjectSetString(object, key, bsNumber(1));
    bsObjectSetString(object, key, bsNumber(2));
    ASSERT_DOUBLE_EQ(bsObjectGet(object, longKey).u.number, 2);
    bsRelease(key);
    bsRelease(object);
}


TEST(value_object_pool)
{
    /* Overflow the recycled-object pool so a free actually returns memory */
    enum { COUNT = 8193 };
    BSValue objects[COUNT];
    for (int ix = 0; ix < COUNT; ix++) {
        objects[ix] = bsObjectNew();
    }
    for (int ix = 0; ix < COUNT; ix++) {
        bsRelease(objects[ix]);
    }
}


TEST(value_object_tree)
{
    /* Insert enough keys, in sorted and reverse order, to exercise both treap rotations */
    BSValue object = bsObjectNew();
    char key[16];
    for (int ix = 0; ix < 200; ix++) {
        snprintf(key, sizeof(key), "k%03d", ix);
        bsObjectSet(object, key, bsNumber(ix));
    }
    for (int ix = 400; ix >= 200; ix--) {
        snprintf(key, sizeof(key), "k%03d", ix);
        bsObjectSet(object, key, bsNumber(ix));
    }
    ASSERT_INT_EQ(bsObjectCount(object), 401);
    for (int ix = 0; ix <= 400; ix++) {
        snprintf(key, sizeof(key), "k%03d", ix);
        ASSERT_DOUBLE_EQ(bsObjectGet(object, key).u.number, ix);
    }

    /* Sorted iteration returns the keys in order */
    BSValue keys = bsObjectKeysSorted(object);
    ASSERT_INT_EQ(bsArrayCount(keys), 401);
    ASSERT_VALUE_STRING(bsRetain(bsArrayGet(keys, 0)), "k000");
    ASSERT_VALUE_STRING(bsRetain(bsArrayGet(keys, 400)), "k400");
    bsRelease(keys);

    /* Delete every key */
    for (int ix = 0; ix <= 400; ix++) {
        snprintf(key, sizeof(key), "k%03d", ix);
        ASSERT_TRUE(bsObjectDelete(object, key));
    }
    ASSERT_INT_EQ(bsObjectCount(object), 0);
    bsRelease(object);
}


TEST(value_datetime)
{
    int64_t milliseconds = bsDatetimeFromParts(2026, 8, 6, 7, 30, 15, 250);
    BSValue value = bsDatetime(milliseconds);
    ASSERT_STR_EQ(bsValueTypeString(value), "datetime");
    ASSERT_TRUE(bsValueBoolean(value));

    BSDatetimeParts parts;
    bsDatetimeParts(milliseconds, &parts);
    ASSERT_INT_EQ(parts.year, 2026);
    ASSERT_INT_EQ(parts.month, 8);
    ASSERT_INT_EQ(parts.day, 6);
    ASSERT_INT_EQ(parts.hour, 7);
    ASSERT_INT_EQ(parts.minute, 30);
    ASSERT_INT_EQ(parts.second, 15);
    ASSERT_INT_EQ(parts.millisecond, 250);

    /* Component normalization */
    bsDatetimeParts(bsDatetimeFromParts(2026, 13, 1, 0, 0, 0, 0), &parts);
    ASSERT_INT_EQ(parts.year, 2027);
    ASSERT_INT_EQ(parts.month, 1);
    bsDatetimeParts(bsDatetimeFromParts(2026, 0, 1, 0, 0, 0, 0), &parts);
    ASSERT_INT_EQ(parts.year, 2025);
    ASSERT_INT_EQ(parts.month, 12);
    bsDatetimeParts(bsDatetimeFromParts(2026, 1, 32, 0, 0, 0, 0), &parts);
    ASSERT_INT_EQ(parts.month, 2);
    ASSERT_INT_EQ(parts.day, 1);
    bsDatetimeParts(bsDatetimeFromParts(2026, 1, 1, 25, 61, 61, 1001), &parts);
    ASSERT_INT_EQ(parts.day, 2);
    ASSERT_INT_EQ(parts.hour, 2);
    ASSERT_INT_EQ(parts.minute, 2);
    ASSERT_INT_EQ(parts.second, 2);
    ASSERT_INT_EQ(parts.millisecond, 1);
    bsDatetimeParts(bsDatetimeFromParts(2026, 1, 1, -1, -1, -1, -1), &parts);
    ASSERT_INT_EQ(parts.year, 2025);
    ASSERT_INT_EQ(parts.month, 12);
    ASSERT_INT_EQ(parts.day, 31);

    /* Negative milliseconds - before the epoch */
    bsDatetimeParts(bsDatetimeFromParts(1969, 12, 31, 23, 59, 59, 500), &parts);
    ASSERT_INT_EQ(parts.year, 1969);
    ASSERT_INT_EQ(parts.millisecond, 500);

    ASSERT_TRUE(bsDatetimeNow() > 0);
    ASSERT_TRUE(bsDatetimeToday() > 0);
}


TEST(value_datetime_parse)
{
    int64_t milliseconds = 0;
    BSDatetimeParts parts;

    ASSERT_TRUE(bsDatetimeParse("2026-08-06", 10, &milliseconds));
    bsDatetimeParts(milliseconds, &parts);
    ASSERT_INT_EQ(parts.year, 2026);
    ASSERT_INT_EQ(parts.month, 8);
    ASSERT_INT_EQ(parts.day, 6);
    ASSERT_INT_EQ(parts.hour, 0);

    ASSERT_TRUE(bsDatetimeParse("2026-08-06T07:30:00Z", 20, &milliseconds));
    ASSERT_TRUE(bsDatetimeParse("2026-08-06T07:30:00.123Z", 24, &milliseconds));
    ASSERT_TRUE(bsDatetimeParse("2026-08-06T07:30:00+02:00", 25, &milliseconds));
    ASSERT_TRUE(bsDatetimeParse("2026-08-06T07:30:00-02:00", 25, &milliseconds));
    ASSERT_TRUE(bsDatetimeParse("2026-08-06T24:00:00Z", 20, &milliseconds));
    ASSERT_TRUE(bsDatetimeParse("2026-08-06T07:30:00.1Z", 22, &milliseconds));
    ASSERT_TRUE(bsDatetimeParse("2026-08-06T07:30:00.123456Z", 27, &milliseconds));

    ASSERT_FALSE(bsDatetimeParse("bad", 3, &milliseconds));
    ASSERT_FALSE(bsDatetimeParse("2026/08/06", 10, &milliseconds));
    ASSERT_FALSE(bsDatetimeParse("202x-08-06", 10, &milliseconds));
    ASSERT_FALSE(bsDatetimeParse("2026-13-06", 10, &milliseconds));
    ASSERT_FALSE(bsDatetimeParse("2026-08-32", 10, &milliseconds));
    ASSERT_FALSE(bsDatetimeParse("2026-08-06X07:30:00Z", 20, &milliseconds));
    ASSERT_FALSE(bsDatetimeParse("2026-08-06T0x:30:00Z", 20, &milliseconds));
    ASSERT_FALSE(bsDatetimeParse("2026-08-06T25:30:00Z", 20, &milliseconds));
    ASSERT_FALSE(bsDatetimeParse("2026-08-06T07:60:00Z", 20, &milliseconds));
    ASSERT_FALSE(bsDatetimeParse("2026-08-06T07:30:60Z", 20, &milliseconds));
    ASSERT_FALSE(bsDatetimeParse("2026-08-06T24:01:00Z", 20, &milliseconds));
    ASSERT_FALSE(bsDatetimeParse("2026-08-06T07:30:00", 19, &milliseconds));
    ASSERT_FALSE(bsDatetimeParse("2026-08-06T07:30:00.Z", 21, &milliseconds));
    ASSERT_FALSE(bsDatetimeParse("2026-08-06T07:30:00.1234567Z", 28, &milliseconds));
    ASSERT_FALSE(bsDatetimeParse("2026-08-06T07:30:00Zx", 21, &milliseconds));
    ASSERT_FALSE(bsDatetimeParse("2026-08-06T07:30:00+2:00", 24, &milliseconds));
    ASSERT_FALSE(bsDatetimeParse("2026-08-06T07:30:00+0x:00", 25, &milliseconds));
    ASSERT_FALSE(bsDatetimeParse("2026-08-06T07:30:00+24:00", 25, &milliseconds));
    ASSERT_FALSE(bsDatetimeParse("2026-08-06T07:30:00+02:60", 25, &milliseconds));
}


TEST(value_datetime_string)
{
    /* The formatted datetime round-trips through the ISO parser */
    int64_t milliseconds = bsDatetimeFromParts(2026, 8, 6, 7, 30, 15, 0);
    BSValue text = bsValueString(bsDatetime(milliseconds));
    int64_t parsed = 0;
    ASSERT_TRUE(bsDatetimeParse(bsStringData(text), bsStringSize(text), &parsed));
    ASSERT_TRUE(parsed == milliseconds);
    ASSERT_INT_EQ(bsStringSize(text), 25);
    bsRelease(text);

    milliseconds = bsDatetimeFromParts(2026, 8, 6, 7, 30, 15, 250);
    text = bsValueString(bsDatetime(milliseconds));
    ASSERT_TRUE(bsDatetimeParse(bsStringData(text), bsStringSize(text), &parsed));
    ASSERT_TRUE(parsed == milliseconds);
    ASSERT_INT_EQ(bsStringSize(text), 29);
    bsRelease(text);
}


TEST(value_compare)
{
    BSValue number1 = bsNumber(1);
    BSValue number2 = bsNumber(2);
    BSValue string1 = bsStringNew("a");
    BSValue string2 = bsStringNew("b");

    ASSERT_INT_EQ(bsValueCompare(bsNull(), bsNull()), 0);
    ASSERT_INT_EQ(bsValueCompare(bsNull(), number1), -1);
    ASSERT_INT_EQ(bsValueCompare(number1, bsNull()), 1);
    ASSERT_INT_EQ(bsValueCompare(number1, number2), -1);
    ASSERT_INT_EQ(bsValueCompare(number2, number1), 1);
    ASSERT_INT_EQ(bsValueCompare(number1, number1), 0);
    ASSERT_INT_EQ(bsValueCompare(string1, string2), -1);
    ASSERT_INT_EQ(bsValueCompare(string2, string1), 1);
    ASSERT_INT_EQ(bsValueCompare(string1, string1), 0);
    ASSERT_INT_EQ(bsValueCompare(bsBoolean(false), bsBoolean(true)), -1);
    ASSERT_INT_EQ(bsValueCompare(bsBoolean(true), bsBoolean(true)), 0);
    ASSERT_INT_EQ(bsValueCompare(bsDatetime(1), bsDatetime(2)), -1);
    ASSERT_INT_EQ(bsValueCompare(bsDatetime(2), bsDatetime(1)), 1);
    ASSERT_INT_EQ(bsValueCompare(bsDatetime(1), bsDatetime(1)), 0);

    /* Different types compare by type name */
    ASSERT_INT_EQ(bsValueCompare(number1, string1), -1);
    ASSERT_INT_EQ(bsValueCompare(string1, number1), 1);
    ASSERT_INT_EQ(bsValueCompare(bsBoolean(true), bsBoolean(true)), 0);

    /* Arrays compare element-wise, then by length */
    BSValue array1 = bsArrayNew();
    BSValue array2 = bsArrayNew();
    ASSERT_INT_EQ(bsValueCompare(array1, array2), 0);
    bsArrayPush(array1, bsNumber(1));
    ASSERT_INT_EQ(bsValueCompare(array1, array2), 1);
    ASSERT_INT_EQ(bsValueCompare(array2, array1), -1);
    bsArrayPush(array2, bsNumber(2));
    ASSERT_INT_EQ(bsValueCompare(array1, array2), -1);

    /* Objects compare by sorted key/value pairs */
    BSValue object1 = bsObjectNew();
    BSValue object2 = bsObjectNew();
    ASSERT_INT_EQ(bsValueCompare(object1, object2), 0);
    bsObjectSet(object1, "b", bsNumber(2));
    bsObjectSet(object1, "a", bsNumber(1));
    bsObjectSet(object2, "a", bsNumber(1));
    bsObjectSet(object2, "b", bsNumber(2));
    ASSERT_INT_EQ(bsValueCompare(object1, object2), 0);
    bsObjectSet(object2, "b", bsNumber(3));
    ASSERT_INT_EQ(bsValueCompare(object1, object2), -1);
    bsObjectSet(object2, "c", bsNumber(4));
    ASSERT_INT_EQ(bsValueCompare(object1, object2), -1);
    BSValue object3 = bsObjectNew();
    bsObjectSet(object3, "z", bsNumber(1));
    ASSERT_INT_EQ(bsValueCompare(object1, object3), -1);

    bsRelease(number1);
    bsRelease(number2);
    bsRelease(string1);
    bsRelease(string2);
    bsRelease(array1);
    bsRelease(array2);
    bsRelease(object1);
    bsRelease(object2);
    bsRelease(object3);
}


TEST(value_is)
{
    BSValue array1 = bsArrayNew();
    BSValue array2 = bsArrayNew();
    ASSERT_TRUE(bsValueIs(bsNumber(1), bsNumber(1)));
    ASSERT_FALSE(bsValueIs(bsNumber(1), bsNumber(2)));
    ASSERT_TRUE(bsValueIs(bsNull(), bsNull()));
    ASSERT_FALSE(bsValueIs(bsNull(), bsNumber(1)));
    ASSERT_TRUE(bsValueIs(bsBoolean(true), bsBoolean(true)));
    ASSERT_FALSE(bsValueIs(bsBoolean(true), bsBoolean(false)));
    ASSERT_TRUE(bsValueIs(bsDatetime(5), bsDatetime(5)));
    ASSERT_FALSE(bsValueIs(bsDatetime(5), bsDatetime(6)));
    ASSERT_TRUE(bsValueIs(array1, array1));
    ASSERT_FALSE(bsValueIs(array1, array2));
    bsRelease(array1);
    bsRelease(array2);
}


TEST(value_retain_release)
{
    BSValue string = bsStringNew("abc");
    BSValue array = bsArrayNew();
    BSValue object = bsObjectNew();
    ASSERT_INT_EQ(string.u.string->refcount, 1);
    bsRetain(string);
    ASSERT_INT_EQ(string.u.string->refcount, 2);
    bsRelease(string);
    ASSERT_INT_EQ(string.u.string->refcount, 1);

    bsRetain(array);
    bsRelease(array);
    bsRetain(object);
    bsRelease(object);

    /* Immediate values ignore retain and release */
    bsRetain(bsNumber(1));
    bsRelease(bsNumber(1));
    bsRetain(bsNull());
    bsRelease(bsNull());

    /* bsAssign releases the previous value */
    BSValue target = bsRetain(string);
    bsAssign(&target, bsNumber(1));
    ASSERT_INT_EQ(target.type, BS_NUMBER);
    ASSERT_INT_EQ(string.u.string->refcount, 1);

    bsRelease(string);
    bsRelease(array);
    bsRelease(object);
}


static BSValue bsTestFunctionFn(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    (void) options;
    (void) data;
    return argCount != 0 ? bsRetain(args[0]) : bsNull();
}


static int bsTestFunctionDataFreed = 0;

static void bsTestFunctionDataFree(void *data)
{
    (void) data;
    bsTestFunctionDataFreed++;
}


TEST(value_function)
{
    BSValue function = bsFunctionNew("test", bsTestFunctionFn, NULL, NULL);
    ASSERT_STR_EQ(bsValueTypeString(function), "function");
    ASSERT_VALUE_STRING(bsRetain(function), "<function>");
    ASSERT_VALUE_KEEP(function, "\"<function>\"");
    ASSERT_TRUE(bsValueBoolean(function));

    BSValue args[1] = {bsNumber(7)};
    BSOptions *options = bsOptionsNew();
    ASSERT_VALUE(bsFunctionCall(function, args, 1, options), "7");
    ASSERT_VALUE(bsFunctionCall(function, NULL, 0, options), "null");

    /* Calling a non-function value returns null */
    ASSERT_VALUE(bsFunctionCall(bsNumber(1), NULL, 0, options), "null");
    bsOptionsFree(options);
    bsRelease(function);

    /* The closure data destructor runs on release */
    bsTestFunctionDataFreed = 0;
    BSValue closure = bsFunctionNew("closure", bsTestFunctionFn, (void *) &bsTestFunctionDataFreed,
                                    bsTestFunctionDataFree);
    bsRelease(closure);
    ASSERT_INT_EQ(bsTestFunctionDataFreed, 1);
}


TEST(value_regex)
{
    char error[BS_REGEX_ERROR_MAX];
    BSValue regex = bsRegexNew("a+", 2, 0, error, sizeof(error));
    ASSERT_STR_EQ(error, "");
    ASSERT_STR_EQ(bsValueTypeString(regex), "regex");
    ASSERT_VALUE_STRING(bsRetain(regex), "<regex>");
    ASSERT_VALUE_KEEP(regex, "null");
    ASSERT_TRUE(bsValueBoolean(regex));
    bsRetain(regex);
    bsRelease(regex);
    bsRelease(regex);
}


static bool bsTestIterStop(BSValue key, BSValue item, void *data)
{
    (void) key;
    (void) item;
    size_t *count = data;
    (*count)++;
    return false;
}


TEST(value_object_iterate)
{
    BSValue object = bsObjectNew();
    bsObjectSet(object, "a", bsNumber(1));
    bsObjectSet(object, "b", bsNumber(2));
    bsObjectSet(object, "c", bsNumber(3));

    /* An iterator that stops early */
    size_t count = 0;
    ASSERT_FALSE(bsObjectIter(object, bsTestIterStop, &count));
    ASSERT_INT_EQ(count, 1);
    count = 0;
    ASSERT_FALSE(bsObjectIterSorted(object, bsTestIterStop, &count));
    ASSERT_INT_EQ(count, 1);

    /* Stop after descending a left child so the in-order walk unwinds from the left */
    BSValue leftObject = bsObjectNew();
    bsObjectSet(leftObject, "m", bsNumber(1));
    bsObjectSet(leftObject, "a", bsNumber(2));
    count = 0;
    ASSERT_FALSE(bsObjectIterSorted(leftObject, bsTestIterStop, &count));
    ASSERT_INT_EQ(count, 1);
    bsRelease(leftObject);

    /* Non-object values iterate as empty */
    count = 0;
    ASSERT_TRUE(bsObjectIter(bsNumber(1), bsTestIterStop, &count));
    ASSERT_TRUE(bsObjectIterSorted(bsNumber(1), bsTestIterStop, &count));
    ASSERT_INT_EQ(count, 0);
    bsRelease(object);
}


static bool bsTestIterStopDeep(BSValue key, BSValue item, void *data)
{
    (void) item;
    size_t *count = data;
    (*count)++;
    /* Stop on a key that sorts in the middle, so the sorted walk unwinds from a right subtree */
    return strcmp(bsStringData(key), "k050") != 0;
}


TEST(value_object_iterate_deep)
{
    BSValue object = bsObjectNew();
    char key[16];
    for (int ix = 0; ix < 100; ix++) {
        snprintf(key, sizeof(key), "k%03d", ix);
        bsObjectSet(object, key, bsNumber(ix));
    }
    size_t count = 0;
    ASSERT_FALSE(bsObjectIterSorted(object, bsTestIterStopDeep, &count));
    ASSERT_INT_EQ(count, 51);
    bsRelease(object);
}


TEST(value_object_node_pool)
{
    /* Overflow the recycled-node pool so further frees go to the allocator */
    BSValue object = bsObjectNew();
    char key[16];
    for (int ix = 0; ix < 1100; ix++) {
        snprintf(key, sizeof(key), "p%04d", ix);
        bsObjectSet(object, key, bsNumber(ix));
    }
    ASSERT_INT_EQ(bsObjectCount(object), 1100);
    bsRelease(object);
}


TEST(value_header_pools)
{
    /* Overflow the recycled array and object header pools */
    BSValue arrays = bsArrayNewCapacity(1100);
    BSValue objects = bsArrayNewCapacity(1100);
    for (int ix = 0; ix < 1100; ix++) {
        bsArrayPush(arrays, bsArrayNew());
        bsArrayPush(objects, bsObjectNew());
    }
    ASSERT_INT_EQ(bsArrayCount(arrays), 1100);
    ASSERT_INT_EQ(bsArrayCount(objects), 1100);
    bsRelease(arrays);
    bsRelease(objects);
}


TEST(value_object_delete_rotations)
{
    /*
     * Delete interior nodes in a scattered order so the treap's remove rotates both ways on its
     * way down to a leaf
     */
    BSValue object = bsObjectNew();
    char key[16];
    for (int ix = 0; ix < 300; ix++) {
        snprintf(key, sizeof(key), "k%03d", (ix * 7919) % 300);
        bsObjectSet(object, key, bsNumber(ix));
    }
    ASSERT_INT_EQ(bsObjectCount(object), 300);
    for (int ix = 0; ix < 300; ix++) {
        snprintf(key, sizeof(key), "k%03d", (ix * 104729) % 300);
        ASSERT_TRUE(bsObjectDelete(object, key));
    }
    ASSERT_INT_EQ(bsObjectCount(object), 0);
    bsRelease(object);
}


TEST(value_json_wrapper)
{
    BSValue object = bsObjectNew();
    bsObjectSet(object, "a", bsNumber(1));
    ASSERT_VALUE_STRING(bsValueJSON(object, 0), "{\"a\":1}");
    ASSERT_VALUE_STRING(bsValueJSON(object, 2), "{\n  \"a\": 1\n}");
    bsRelease(object);
}
