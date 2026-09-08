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
    return bsGzipUncompress(gzipBytes, gzipSize);
}


TEST(include_registry)
{
    ASSERT_INT_EQ(bsIncludeCount(), BS_INCLUDE_COUNT);

    /* Every bundled include has a name and decodes to a JSON script model */
    for (size_t ix = 0; ix < bsIncludeCount(); ix++) {
        const char *name = bsIncludeName(ix);
        ASSERT_NOT_NULL(name);
        const char *source = bsIncludeSource(name);
        ASSERT_NOT_NULL(source);
        ASSERT_INT_EQ(source[0], '{');
    }

    /* An out-of-range index and an unknown name */
    ASSERT_NULL(bsIncludeName(bsIncludeCount()));
    ASSERT_NULL(bsIncludeSource("no-such-include.bare"));

    /* The decoded model is cached, so a second lookup returns the same buffer */
    const char *first = bsIncludeSource("unittest.bare");
    const char *second = bsIncludeSource("unittest.bare");
    ASSERT_TRUE(first == second);
}


TEST(include_decode)
{
    /* Every bundled include's decoded model parses as JSON and converts to a script */
    for (size_t ix = 0; ix < bsIncludeCount(); ix++) {
        const char *name = bsIncludeName(ix);
        const char *source = bsIncludeSource(name);
        const char *error = NULL;
        BSValue model = bsJSONDecode(source, strlen(source), &error);
        if (error != NULL) {
            bsTestFail(__FILE__, __LINE__, "%s: %s", name, error);
        }
        BSScript *script = bsScriptFromModel(model, name);
        if (script == NULL) {
            bsTestFail(__FILE__, __LINE__, "%s: invalid model", name);
        }
        bsTestPass();
        bsScriptRelease(script);
        bsRelease(model);
    }
}


TEST(include_stub_accessors)
{
    /* Every generated stub accessor returns its include's model */
    for (size_t ix = 0; ix < BS_INCLUDE_COUNT; ix++) {
        ASSERT_TRUE(bsIncludeSourceStubs[ix]() == bsIncludeSource(bsIncludeName(ix)));
    }

    /* The named stubs resolve to the same models */
    ASSERT_TRUE(bsIncludeSourceArgs() == bsIncludeSource("args.bare"));
    ASSERT_TRUE(bsIncludeSourceUnittest() == bsIncludeSource("unittest.bare"));
    ASSERT_TRUE(bsIncludeSourceBarescriptParser() == bsIncludeSource("barescriptParser.bare"));
    ASSERT_TRUE(bsIncludeSourceBarescriptLint() == bsIncludeSource("barescriptLint.bare"));
    ASSERT_TRUE(bsIncludeSourceMarkdownUp() == bsIncludeSource("markdownUp.bare"));
    ASSERT_TRUE(bsIncludeSourceSchema() == bsIncludeSource("schema.bare"));
    ASSERT_TRUE(bsIncludeSourceUrl() == bsIncludeSource("url.bare"));
    ASSERT_TRUE(bsIncludeSourceQrcode() == bsIncludeSource("qrcode.bare"));
    ASSERT_TRUE(bsIncludeSourceGzip() == bsIncludeSource("gzip.bare"));
    ASSERT_TRUE(bsIncludeSourceBase64() == bsIncludeSource("base64.bare"));
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
    ASSERT_NULL(bsIncludeSourceUrl());
    ASSERT_NULL(bsIncludeSource("url.bare"));
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
        NULL
    };
    for (size_t ix = 0; invalid[ix] != NULL; ix++) {
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
    ASSERT_NULL(bsIncludeSourceDecode(ixUrl));
    bsIncludeSources[ixUrl].gzip = NULL;
    ASSERT_NULL(bsIncludeSourceDecode(ixUrl));
    bsIncludeSources[ixUrl] = saved;
}
