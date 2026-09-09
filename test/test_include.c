/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The bundled include library unit tests
 */

#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "../src/includeSourceDecode.h"
#include "../src/internal.h"


static size_t bsTestIncludeIndex(const char *name)
{
    size_t ix = 0;
    while (strcmp(bsIncludeName(ix), name) != 0) {
        ix++;
    }
    return ix;
}


/* Inflate base64-encoded gzip test data. The bundled models are
 * raw bytes, so the library has no base64 decoder; this one trusts its input. */
static char *bsTestGzipDecode(const char *text)
{
    size_t size;
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned char gzipBytes[1024];
    size_t gzipSize = 0;
    unsigned bits = 0;
    int count = 0;
    for (const char *ch = text; *ch != '\0' && *ch != '='; ch++) {
        bits = (bits << 6) | (unsigned) (strchr(chars, *ch) - chars);
        if (++count == 4) {
            gzipBytes[gzipSize++] = (unsigned char) (bits >> 16);
            gzipBytes[gzipSize++] = (unsigned char) (bits >> 8);
            gzipBytes[gzipSize++] = (unsigned char) bits;
            bits = 0;
            count = 0;
        }
    }
    if (count == 2) {
        gzipBytes[gzipSize++] = (unsigned char) (bits >> 4);
    } else if (count == 3) {
        gzipBytes[gzipSize++] = (unsigned char) (bits >> 10);
        gzipBytes[gzipSize++] = (unsigned char) (bits >> 2);
    }
    return (char *) bsGzipUncompress(gzipBytes, gzipSize, &size);
}


TEST(include_registry)
{
    ASSERT_INT_EQ(bsIncludeCount(), BS_INCLUDE_COUNT);

    /* Every bundled include has a name and inflates to a binary script model - the parser and linter
       self-contained version one, the rest version two, referring to the shared strings */
    for (size_t ix = 0; ix < bsIncludeCount(); ix++) {
        const char *name = bsIncludeName(ix);
        ASSERT_NOT_NULL(name);
        size_t size;
        const unsigned char *source = bsIncludeSource(name, &size);
        ASSERT_NOT_NULL(source);
        ASSERT_TRUE(size > 1);
        bool required = strcmp(name, "barescriptParser.bare") == 0 || strcmp(name, "barescriptLint.bare") == 0;
        ASSERT_INT_EQ(source[0], required ? 1 : 2);
    }

    /* An out-of-range index and an unknown name */
    size_t size = 1;
    ASSERT_NULL(bsIncludeName(bsIncludeCount()));
    ASSERT_NULL(bsIncludeSource("no-such-include.bare", &size));
    ASSERT_INT_EQ(size, 0);

    /* The inflated model is cached, so a second lookup returns the same buffer */
    size_t firstSize;
    size_t secondSize;
    const unsigned char *first = bsIncludeSource("unittest.bare", &firstSize);
    const unsigned char *second = bsIncludeSource("unittest.bare", &secondSize);
    ASSERT_TRUE(first == second);
    ASSERT_INT_EQ(firstSize, secondSize);
}


TEST(include_decode)
{
    /* Every bundled include's binary model converts to a script */
    for (size_t ix = 0; ix < bsIncludeCount(); ix++) {
        const char *name = bsIncludeName(ix);
        size_t size;
        const unsigned char *source = bsIncludeSource(name, &size);
        BSScript *script = bsScriptFromModelBinary(source, size, name);
        if (script == NULL) {
            bsTestFail(__FILE__, __LINE__, "%s: invalid model", name);
            continue;
        }
        bsTestPass();
        ASSERT_STR_EQ(bsStringData(script->scriptName), name);
        bsScriptRelease(script);
    }
}


TEST(include_stub_accessors)
{
    /* Every generated stub accessor returns its include's model */
    for (size_t ix = 0; ix < BS_INCLUDE_COUNT; ix++) {
        size_t stubSize;
        size_t size;
        ASSERT_TRUE(bsIncludeSourceStubs[ix](&stubSize) == bsIncludeSource(bsIncludeName(ix), &size));
        ASSERT_INT_EQ(stubSize, size);
    }

    /* The named stubs resolve to the same models */
    size_t size;
    ASSERT_TRUE(bsIncludeSourceArgs(&size) == bsIncludeSource("args.bare", &size));
    ASSERT_TRUE(bsIncludeSourceUnittest(&size) == bsIncludeSource("unittest.bare", &size));
    ASSERT_TRUE(bsIncludeSourceBarescriptParser(&size) == bsIncludeSource("barescriptParser.bare", &size));
    ASSERT_TRUE(bsIncludeSourceBarescriptLint(&size) == bsIncludeSource("barescriptLint.bare", &size));
    ASSERT_TRUE(bsIncludeSourceMarkdownUp(&size) == bsIncludeSource("markdownUp.bare", &size));
    ASSERT_TRUE(bsIncludeSourceSchema(&size) == bsIncludeSource("schema.bare", &size));
    ASSERT_TRUE(bsIncludeSourceUrl(&size) == bsIncludeSource("url.bare", &size));
    ASSERT_TRUE(bsIncludeSourceQrcode(&size) == bsIncludeSource("qrcode.bare", &size));
    ASSERT_TRUE(bsIncludeSourceGzip(&size) == bsIncludeSource("gzip.bare", &size));
    ASSERT_TRUE(bsIncludeSourceBase64(&size) == bsIncludeSource("base64.bare", &size));
}


/* A binary model under construction - a byte at a time, with the encoding's varints and strings */
typedef struct BSTestModel {
    unsigned char bytes[8192];
    size_t size;
} BSTestModel;

static void bsTestModelByte(BSTestModel *model, unsigned byte)
{
    model->bytes[model->size++] = (unsigned char) byte;
}

static void bsTestModelVarint(BSTestModel *model, uint64_t value)
{
    while (value >= 128) {
        bsTestModelByte(model, (unsigned) (value % 128 + 128));
        value /= 128;
    }
    bsTestModelByte(model, (unsigned) value);
}

/* The string table - each string as its length and bytes */
static void bsTestModelStrings(BSTestModel *model, const char *const *strings, size_t count)
{
    bsTestModelVarint(model, count);
    for (size_t ix = 0; ix < count; ix++) {
        size_t length = strlen(strings[ix]);
        bsTestModelVarint(model, length);
        memcpy(model->bytes + model->size, strings[ix], length);
        model->size += length;
    }
}

/* Append bytes as they are */
static void bsTestModelBytes(BSTestModel *model, const unsigned char *bytes, size_t count)
{
    memcpy(model->bytes + model->size, bytes, count);
    model->size += count;
}

#define BS_TEST_MODEL_BYTES(model, ...) \
    do { \
        static const unsigned char bytes[] = {__VA_ARGS__}; \
        bsTestModelBytes((model), bytes, sizeof(bytes)); \
    } while (0)

static void bsTestBinaryModelResult(const BSTestModel *model, const char *expected)
{
    BSScript *script = bsScriptFromModelBinary(model->bytes, model->size, "test.bare");
    if (script == NULL) {
        bsTestFail(__FILE__, __LINE__, "the model did not decode");
        return;
    }
    ASSERT_VALUE(bsTestExecuteScript(script), expected);
    bsScriptRelease(script);
}

static void bsTestModelInvalid(const BSTestModel *model, size_t label)
{
    BSScript *script = bsScriptFromModelBinary(model->bytes, model->size, "test.bare");
    if (script != NULL) {
        bsScriptRelease(script);
        bsTestFail(__FILE__, __LINE__, "expected an invalid model (%zu)", label);
    } else {
        bsTestPass();
    }
}


TEST(include_model_binary)
{
    /*
     * function f(a, b...):
     *     x = a + -1
     *     jumpif (!b) done
     *     return 1.5
     *     done:
     *     return if(x > 2, "big", "small", 99)
     * endfunction
     * z = 1.5
     * y = f(3)
     * arrayNew()
     * jump skip
     * skip:
     * return (arrayNew(y, z))
     */
    static const char *const strings[] = {
        "f", "a", "b", "x", "+", "-", "done", "!", "if", ">", "big", "small", "y", "z", "1.5", "arrayNew", "skip"
    };
    BSTestModel model = {.size = 0};
    bsTestModelByte(&model, 1);
    bsTestModelStrings(&model, strings, sizeof(strings) / sizeof(strings[0]));
    bsTestModelVarint(&model, 7);
    BS_TEST_MODEL_BYTES(&model, 5, 1, 1, 1, 2, 2, 3, 5);
    BS_TEST_MODEL_BYTES(&model, 1, 2, 4, 6, 5, 4, 2, 7, 6, 1, 2);
    BS_TEST_MODEL_BYTES(&model, 2, 3, 7, 7, 8, 4, 3);
    BS_TEST_MODEL_BYTES(&model, 3, 4, 2, 15);
    BS_TEST_MODEL_BYTES(&model, 4, 5, 7);
    BS_TEST_MODEL_BYTES(&model, 3, 6, 5, 9, 4, 6, 10, 4, 4, 1, 4, 3, 11, 3, 12, 1);
    bsTestModelVarint(&model, 198);
    BS_TEST_MODEL_BYTES(&model, 1, 8, 14, 2, 15);
    BS_TEST_MODEL_BYTES(&model, 1, 9, 13, 5, 1, 1, 1, 6);
    BS_TEST_MODEL_BYTES(&model, 1, 10, 0, 5, 16, 0);
    BS_TEST_MODEL_BYTES(&model, 2, 11, 17, 0);
    BS_TEST_MODEL_BYTES(&model, 4, 12, 17);
    BS_TEST_MODEL_BYTES(&model, 3, 13, 8, 5, 16, 2, 4, 13, 4, 14);
    bsTestBinaryModelResult(&model, "[\"small\",1.5]");

    /* A negative integer, an unnamed expression statement, and zero line numbers */
    static const char *const negative[] = {"-"};
    model.size = 0;
    bsTestModelByte(&model, 1);
    bsTestModelStrings(&model, negative, 1);
    BS_TEST_MODEL_BYTES(&model, 2, 1, 0, 0, 6, 1, 1, 6, 1, 20, 3, 0, 1, 13);
    bsTestBinaryModelResult(&model, "-7");

    /* An include statement */
    static const char *const includes[] = {"args.bare", "systemType", "argsParse"};
    model.size = 0;
    bsTestModelByte(&model, 1);
    bsTestModelStrings(&model, includes, 3);
    BS_TEST_MODEL_BYTES(&model, 2, 6, 1, 1, 1, 1, 3, 2, 5, 2, 1, 4, 3);
    bsTestBinaryModelResult(&model, "\"function\"");

    /* An expression at the nesting limit fails, one just inside it does not - the statement is at
       depth one, its expression at two, so the literal inside 997 groups is at depth 999 */
    for (int groups = 997; groups <= 998; groups++) {
        model.size = 0;
        BS_TEST_MODEL_BYTES(&model, 1, 0, 1, 3, 0);
        for (int ix = 0; ix < groups; ix++) {
            bsTestModelByte(&model, 8);
        }
        BS_TEST_MODEL_BYTES(&model, 1, 2);
        if (groups == 997) {
            bsTestBinaryModelResult(&model, "1");
        } else {
            bsTestModelInvalid(&model, 0);
        }
    }
    model.size = 0;
    BS_TEST_MODEL_BYTES(&model, 1, 1, 1, 'f', 1);
    for (int ix = 0; ix < 999; ix++) {
        BS_TEST_MODEL_BYTES(&model, 5, 0, 1, 0, 0, 1);
    }
    bsTestModelInvalid(&model, 1);
}


TEST(include_model_binary_invalid)
{
    /* Each malformed shape fails the model: the version, a truncated string table, a truncated
       statement list, an unknown statement kind, an unknown expression tag, a required expression
       that is absent, a name that is absent or past the table, a number whose text is not one,
       an unknown binary or unary operator, an empty include, bytes past the statements, a varint
       past ten bytes, a line number past an int, and a malformed statement inside a function */
    static const struct {
        unsigned char bytes[24];
        size_t count;
    } cases[] = {
        {{3, 0, 1, 3, 0, 0}, 6},
        {{1, 1, 5, 'a', 'b'}, 5},
        {{1, 0, 2, 3, 0, 0}, 6},
        {{1, 0, 1, 7, 0}, 5},
        {{1, 0, 1, 3, 0, 9}, 6},
        {{1, 0, 1, 1, 0, 0, 0}, 7},
        {{1, 0, 1, 4, 0, 0}, 6},
        {{1, 0, 1, 4, 0, 1}, 6},
        {{1, 1, 1, 'x', 1, 3, 0, 2, 1}, 9},
        {{1, 1, 1, '?', 1, 3, 0, 6, 1, 1, 0, 1, 0}, 13},
        {{1, 1, 1, '?', 1, 3, 0, 7, 1, 1, 0}, 11},
        {{1, 0, 1, 6, 0, 0}, 6},
        {{1, 0, 1, 3, 0, 0, 0}, 7},
        {{1, 0, 1, 3, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0, 0}, 16},
        {{1, 0, 1, 3, 0x80, 0x80, 0x80, 0x80, 0x08, 0}, 10},
        {{1, 1, 1, 'f', 1, 5, 0, 1, 0, 0, 1, 7, 0}, 13},
        {{1, 0, 1, 5, 0, 1, 0, 0, 0}, 9},
        {{1, 0, 1, 6, 0, 1, 1, 1}, 8},
    };
    for (size_t ix = 0; ix < sizeof(cases) / sizeof(cases[0]); ix++) {
        BSTestModel model = {.size = 0};
        bsTestModelBytes(&model, cases[ix].bytes, cases[ix].count);
        bsTestModelInvalid(&model, ix);
    }

    /* Every prefix of a bundled model fails */
    size_t size;
    const unsigned char *args = bsIncludeSource("args.bare", &size);
    for (size_t prefix = 0; prefix < size; prefix += 7) {
        BSScript *script = bsScriptFromModelBinary(args, prefix, "args.bare");
        ASSERT_NULL(script);
    }
}


TEST(include_model_shared)
{
    /*
     * A version 2 model reads its own strings and the shared table's through tagged references:
     * one local string, then two return statements of variables - the local string, and the
     * shared table's first - whose names the chunk's names table holds
     */
    BSTestModel model = {.size = 0};
    BS_TEST_MODEL_BYTES(&model, 2, 1, 1, 'x', 2, 3, 1, 4, 1, 3, 1, 4, 2);
    BSScript *script = bsScriptFromModelBinary(model.bytes, model.size, "test.bare");
    ASSERT_NOT_NULL(script);
    BSValue shared;
    ASSERT_TRUE(bsIncludeSharedString(0, &shared));
    ASSERT_INT_EQ(script->code.nameCount, 2);
    ASSERT_STR_EQ(bsStringData(script->code.names[0]), "x");
    ASSERT_STR_EQ(bsStringData(script->code.names[1]), bsStringData(shared));
    bsScriptRelease(script);

    /* A shared reference past the table, and a version past 2 */
    BSTestModel past = {.size = 0};
    BS_TEST_MODEL_BYTES(&past, 2, 0, 1, 3, 1, 4, 0x82, 0x80, 0x80, 0x01);
    bsTestModelInvalid(&past, 0);
    BSTestModel version = {.size = 0};
    BS_TEST_MODEL_BYTES(&version, 3, 0, 0);
    bsTestModelInvalid(&version, 1);
    ASSERT_FALSE(bsIncludeSharedString(1000000, &shared));
}


TEST(include_compiled_out)
{
    /* A compiled-out include keeps its registry entry with no model - as here, with its data cleared */
    size_t ix = bsTestIncludeIndex("url.bare");
    bsIncludeCleanup();
    BSIncludeSource saved = bsIncludeSources[ix];
    bsIncludeSources[ix].gzip = NULL;
    bsIncludeSources[ix].gzipSize = 0;
    ASSERT_INT_EQ(bsIncludeCount(), BS_INCLUDE_COUNT);
    ASSERT_STR_EQ(bsIncludeName(ix), "url.bare");
    size_t size;
    ASSERT_NULL(bsIncludeSourceUrl(&size));
    ASSERT_NULL(bsIncludeSource("url.bare", &size));
    ASSERT_NULL(bsIncludeScript("url.bare"));

    /* An include statement for it fails... */
    ASSERT_VALUE(bsTestExecute("include <url.bare>\nreturn urlEncode('a b')"), "null");
    ASSERT_STR_CONTAINS(bsTestErrorText(), "url.bare");

    /* ...unless the system include path serves it */
    bsSystemIncludeRegister("url.bare", "function urlEncode(text):\n    return 'from the path'\nendfunction\n");
    ASSERT_VALUE(bsTestExecute("include <url.bare>\nreturn urlEncode('a b')"), "\"from the path\"");
    bsSystemIncludeClear();
    bsIncludeSources[ix] = saved;
}


TEST(include_script_cache)
{
    ASSERT_NULL(bsIncludeScript("no-such-include.bare"));
    BSScript *first = bsIncludeScript("url.bare");
    ASSERT_TRUE(first != NULL);
    BSScript *second = bsIncludeScript("url.bare");
    ASSERT_TRUE(first == second);
    bsScriptRelease(first);
    bsScriptRelease(second);
}


TEST(include_system_include)
{
    /* A bundled include resolves without a file system */
    ASSERT_VALUE(bsTestExecute("include <unittest.bare>\nreturn systemType(unittestRunTest)"),
                 "\"function\"");

    /* Including the same bundled script twice evaluates it once */
    ASSERT_VALUE(bsTestExecute("include <url.bare>\ninclude <url.bare>\n"
                               "return urlEncodeComponent('a b')"), "\"a%20b\"");

    /* A bundled include with its own includes */
    ASSERT_VALUE(bsTestExecute("include <schemaParser.bare>\n"
                               "return objectHas(objectGet(schemaParse(['struct S', '    int a']), 'S'), "
                               "'struct')"), "true");

    /* Several bundled includes in one statement */
    ASSERT_VALUE(bsTestExecute("include <args.bare>\ninclude <markdown.bare>\n"
                               "return [systemType(argsParse), systemType(markdownParse)]"),
                 "[\"function\",\"function\"]");

    ASSERT_VALUE(bsTestExecute("include <gzip.bare>\n"
                               "return gzipUncompress(gzipCompress([104, 105]))"),
                 "[104,105]");
}


TEST(include_gzip_decode)
{
    static const struct {
        const char *gzip;
        const char *text;
    } cases[] = {
        {"H4sIAAAAAAAC/8tIzcnJBwCGphA2BQAAAA==", "hello"},
        {"H4sIAAAAAAAA/wMAAAAAAAAAAAA=", ""},
        {"H4sIAAAAAAAC/0sEAgBF5ZitBAAAAA==", "aaaa"},   /* a match overlapping its own output */
        {"H4sIAAAAAAAC/0pMAiwxCQCmCtc2BAAAAA==", "abab"},   /* two fixed blocks */
        {"H4sIAAAAAAAC/wXBgQAAAACAINb2h7ikAaYK1zYEAAAA", "abab"},   /* dynamic codes */
        {"H4sIAAAAAAAC/wXBBQEAAACDsKwX+ldgS3cBEc2C7QQAAAA=", "abcd"},   /* dynamic codes with a repeated length */
        {"H4sIAAAAAAAC/wXBAYEkSRDEMKyOrJ69f/4AJPX/fwDUIigdAwAAAA==", "aji"},    /* dynamic codes longer than the decode table */
    };
    for (size_t ix = 0; ix < sizeof(cases) / sizeof(cases[0]); ix++) {
        char *decoded = bsTestGzipDecode(cases[ix].gzip);
        ASSERT_STR_EQ(decoded, cases[ix].text);
        free(decoded);
    }
}


TEST(include_gzip_invalid)
{
    /* Not gzip, a wrong size, and other shapes of gzip stream than the one the compressor writes */
    static const char *const invalid[] = {
        "YWJj",
        "H4sIAAAAAAAC/8tIzcnJBwCGphA2BQAAAQ==",
        "H4sIAAAAAAAE/wEFAPr/aGVsbG+GphA2BQAAAA==", /* a stored block */
        "H4sICAAAAAAA/3gAAwAAAAAAAAAAAA==",             /* FNAME */
        "H4sIBAAAAAAA/wIAQUIDAAAAAAAAAAAA",             /* FEXTRA */
        "H4sIEAAAAAAA/2hpAAMAAAAAAAAAAAA=",             /* FCOMMENT */
        "H4sIAgAAAAAA/5DJAwAAAAAAAAAAAA==",             /* FHCRC */
        "H4sIAQAAAAAA/wMAAAAAAAAAAAA=",                 /* FTEXT */
        "H4sIAAAAAAAA/wAAAP//AwAAAAAAAAAAAA==",         /* two blocks */
        "H4sIAAAAAAAA/ztxAggAAAAAAgAAAA==",                 /* the input ends inside a distance code */
        "H4sIAAAAAAAA/wcAAAAAAAAAAA==",
        "H4sIAAAAAAAA//UAAAAAAAAAAAAA",
        "H4sIIAAAAAAA/wMAAAAAAAAAAAA=",
        "H4sAAAAAAAAA/wMAAAAAAAAAAAA=",
        "HosIAAAAAAAA/wMAAAAAAAAAAAA=",
        "H4sIBAAAAAAA/wA=",
        "H4sICAAAAAAA/2Fi",
        "H4sIEAAAAAAA/2E=",
        "H4sIAgAAAAAA/wA=",
        "H4sIAAAAAAAA/wMAAAA=",
        "H4sIAAAAAAAA/wMAAAAAAAAAAA==",
        "H4sIAAAAAAAA/wEBAP//AAAAAAAAAAA=",
        "H4sIAAAAAAAA/wEFAPr/YQAAAAAAAAAA",
        "H4sIAAAAAAAA/8tIzcnJBwAAAAAAAAAAAA==",
        "H4sIAAAAAAAA/wAAAAAAAAAA",
        "H4sICAAAAAAA/2FiAAAAAAAAAAA=",
        "H4sIEAAAAAAA/wEBAQEB",
        "H4sIAgAAAAAA/wE=",
        "H4sIAAAAAAAC/wemCtc2BAAAAA==",                     /* a block of type 3 */
        "H4sIAAAAAAAC/wEEAPv/YWJhYqYK1zYEAAAA",                 /* a stored block */
        "H4sIAAAAAAAC/wMAAAAAAAAAAA==",                     /* the input ends inside a literal code */
        "H4sIAAAAAAAC/0tMAgBtSIOeAQAAAA==",                     /* a literal past the declared size */
        "H4sIAAAAAAAC/xsDpgrXNgQAAAA=",                     /* the fixed code's literal 286 */
        "H4sIAAAAAAAC/0sEPqYK1zYEAAAA",                     /* the fixed code's distance 30 */
        "H4sIAAAAAAAC/0tEAqYK1zYEAAAA",                     /* the input ends inside a length's extra bits */
        "H4sIAAAAAAAC/0sEEqYK1zYEAAAA",                     /* the input ends inside a distance's extra bits */
        "H4sIAAAAAAAC/wMCAKYK1zYEAAAA",                     /* a match before any output */
        "H4sIAAAAAAAC/0sEAgBF5ZitAQAAAA==",                 /* a match past the declared size */
        "H4sIAAAAAAAC//XBgQAAAACAINb2h7ikAaYK1zYEAAAA",         /* a dynamic block with more than 286 literal codes */
        "H4sIAAAAAAAC/wXBBQEAAACDMMkqDaYK1zYEAAAA",             /* a dynamic block repeating a length before any */
        "H4sIAAAAAAAC/wXBgQAAAACAIH9/f2mmCtc2BAAAAA==",         /* a dynamic block's lengths past their count */
        "H4sIAAAAAAAC/wXBgQAAAACAINbK36HSpgrXNgQAAAA=",         /* an over-subscribed literal code */
        "H4sIAAAAAAAC/wXBgQAAAABAEFb9ISINpgrXNgQAAAA=",         /* an over-subscribed code length code */
        "H4sIAAAAAAAC/wWmCtc2BAAAAA==",                     /* the input ends inside a dynamic block's header */
        "H4sIAAAAAAAC/wXBAYEkSRDEMKyOrJ69f/4AJPUBQ7636AEAAAA=",  /* the input ends inside a code longer than the decode table */
        "H4sIAAAAAAAC/ztx4sSJEyeQyVSSpggAAAA=",         /* the input ends inside a length's extra bits, on a byte */
        "H4sIAAAAAAAC/zuRCCTJVJKmCAAAAA==",             /* the input ends inside a distance's extra bits, on a byte */
        "H4sIAAAAAAAC/wXAJEnJVJKmCAAAAA==",             /* the input ends inside the code length code's lengths */
        "H4sIAAAAAAAC/wUgJAnJVJKmCAAAAA==",             /* the input ends before a dynamic block's run-length symbols */
        "H4sIAAAAAAAC/wXBgQAABAAAINb9JS7JVJKmCAAAAA==",     /* the input ends inside a long code no shorter code matches */
        "H4sIAAAAAAAC/wXBgQAABAAAINb9JS7//8lUkqYIAAAA"  /* bits no code of an incomplete code matches */
    };
    for (size_t ix = 0; ix < sizeof(invalid) / sizeof(invalid[0]); ix++) {
        char *decoded = bsTestGzipDecode(invalid[ix]);
        ASSERT_NULL(decoded);
        free(decoded);
    }

    /* A missing or empty gzip blob fails to decode - patched into a registry entry, then restored */
    size_t ixUrl = bsTestIncludeIndex("url.bare");
    bsIncludeCleanup();
    BSIncludeSource saved = bsIncludeSources[ixUrl];
    static const unsigned char dummyGzip[1] = {0};
    bsIncludeSources[ixUrl].gzip = dummyGzip;
    bsIncludeSources[ixUrl].gzipSize = 0;
    size_t size;
    ASSERT_NULL(bsIncludeSourceDecode(ixUrl, &size));
    bsIncludeSources[ixUrl].gzip = NULL;
    ASSERT_NULL(bsIncludeSourceDecode(ixUrl, &size));
    bsIncludeSources[ixUrl] = saved;
}
