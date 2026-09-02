/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The bundled include library unit tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "../src/includeSourceDecode.h"


static const char *bsTestGzipDecode(const char *const *chunks)
{
    BSIncludeSource source = {"test.bare", chunks, NULL};
    return bsIncludeSourceDecode(&source);
}


TEST(include_registry)
{
    ASSERT_INT_EQ(bsIncludeCount(), BS_INCLUDE_COUNT);
    ASSERT_TRUE(bsIncludeCount() > 0);

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
            bsRelease(model);
            return;
        }
        BSScript *script = bsScriptFromModel(model, name);
        if (script == NULL) {
            bsTestFail(__FILE__, __LINE__, "%s: invalid model", name);
            bsRelease(model);
            return;
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
    static const char *const hello[] = {"H4sIAAAAAAAC/8tIzcnJBwCGphA2BQAAAA==", NULL};
    static const char *const helloChunks[] = {"H4sIAAAAAAAC/8tIzcnJB", "wCGphA2BQAAAA==", NULL};
    static const char *const empty[] = {"H4sIAAAAAAAA/wMAAAAAAAAAAAA=", NULL};
    static const char *const stored[] = {"H4sIAAAAAAAE/wEFAPr/aGVsbG+GphA2BQAAAA==", NULL};
    static const char *const fname[] = {"H4sICAAAAAAA/3gAAwAAAAAAAAAAAA==", NULL};
    static const char *const fextra[] = {"H4sIBAAAAAAA/wIAQUIDAAAAAAAAAAAA", NULL};
    static const char *const fcomment[] = {"H4sIEAAAAAAA/2hpAAMAAAAAAAAAAAA=", NULL};
    static const char *const fhcrc[] = {"H4sIAgAAAAAA/5DJAwAAAAAAAAAAAA==", NULL};
    static const char *const ftext[] = {"H4sIAQAAAAAA/wMAAAAAAAAAAAA=", NULL};
    static const char *const twoBlocks[] = {"H4sIAAAAAAAA/wAAAP//AwAAAAAAAAAAAA==", NULL};

    BSIncludeSource src = {"t.bare", hello, NULL};
    ASSERT_STR_EQ(bsIncludeSourceDecode(&src), "hello");
    ASSERT_TRUE(bsIncludeSourceDecode(&src) == src.decoded);
    free(src.decoded);

    src.compressed = helloChunks;
    src.decoded = NULL;
    ASSERT_STR_EQ(bsIncludeSourceDecode(&src), "hello");
    free(src.decoded);

    src.compressed = empty;
    src.decoded = NULL;
    ASSERT_STR_EQ(bsIncludeSourceDecode(&src), "");
    free(src.decoded);

    src.compressed = stored;
    src.decoded = NULL;
    ASSERT_STR_EQ(bsIncludeSourceDecode(&src), "hello");
    free(src.decoded);

    src.compressed = fname;
    src.decoded = NULL;
    ASSERT_STR_EQ(bsIncludeSourceDecode(&src), "");
    free(src.decoded);

    src.compressed = fextra;
    src.decoded = NULL;
    ASSERT_STR_EQ(bsIncludeSourceDecode(&src), "");
    free(src.decoded);

    src.compressed = fcomment;
    src.decoded = NULL;
    ASSERT_STR_EQ(bsIncludeSourceDecode(&src), "");
    free(src.decoded);

    src.compressed = fhcrc;
    src.decoded = NULL;
    ASSERT_STR_EQ(bsIncludeSourceDecode(&src), "");
    free(src.decoded);

    src.compressed = ftext;
    src.decoded = NULL;
    ASSERT_STR_EQ(bsIncludeSourceDecode(&src), "");
    free(src.decoded);

    src.compressed = twoBlocks;
    src.decoded = NULL;
    ASSERT_STR_EQ(bsIncludeSourceDecode(&src), "");
    free(src.decoded);
}


TEST(include_gzip_invalid)
{
    static const char *const invalid[][3] = {
        {"!!!!", NULL},
        {"AAA", NULL},
        {"AAA*", NULL},
        {"AA==AAAA", NULL},
        {"AA=A", NULL},
        {"YWJj", NULL},
        {"H4sIAAAAAAAC/8tIzcnJBwCGphA3BQAAAA==", NULL},
        {"H4sIAAAAAAAC/8tIzcnJBwCGphA2BQAAAQ==", NULL},
        {"H4sIAAAAAAAA/wcAAAAAAAAAAA==", NULL},
        {"H4sIAAAAAAAA//UAAAAAAAAAAAAA", NULL},
        {"H4sIIAAAAAAA/wMAAAAAAAAAAAA=", NULL},
        {"H4sAAAAAAAAA/wMAAAAAAAAAAAA=", NULL},
        {"HosIAAAAAAAA/wMAAAAAAAAAAAA=", NULL},
        {"H4sIBAAAAAAA/wA=", NULL},
        {"H4sICAAAAAAA/2Fi", NULL},
        {"H4sIEAAAAAAA/2E=", NULL},
        {"H4sIAgAAAAAA/wA=", NULL},
        {"H4sIAAAAAAAA/wMAAAA=", NULL},
        {"H4sIAAAAAAAA/wMAAAAAAAAAAA==", NULL},
        {"H4sIAAAAAAAA/wEBAP//AAAAAAAAAAA=", NULL},
        {"H4sIAAAAAAAA/wEFAPr/YQAAAAAAAAAA", NULL},
        {"H4sIAAAAAAAA/8tIzcnJBwAAAAAAAAAAAA==", NULL},
        {"H4sIAAAAAAAA/wAAAAAAAAAA", NULL},
        {"H4sIBAAAAAAA/wA=", NULL},
        {"H4sICAAAAAAA/2FiAAAAAAAAAAA=", NULL},
        {"H4sIEAAAAAAA/wEBAQEB", NULL},
        {"H4sIAgAAAAAA/wE=", NULL},
        {NULL}
    };
    for (size_t ix = 0; invalid[ix][0] != NULL; ix++) {
        ASSERT_NULL(bsTestGzipDecode(invalid[ix]));
    }
}
