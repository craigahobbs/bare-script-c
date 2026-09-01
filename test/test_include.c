/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The bundled include library unit tests
 */

#include <stdio.h>
#include <string.h>

#include "test.h"


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
}


TEST(include_phrase_table)
{
    /* The phrase table is complete - 61 phrases plus the "~" escape */
    ASSERT_INT_EQ(bsIncludeSourcePhraseCount, 62);
    for (size_t ix = 0; ix < bsIncludeSourcePhraseCount; ix++) {
        ASSERT_NOT_NULL(bsIncludeSourcePhrases[ix]);
    }
    ASSERT_STR_EQ(bsIncludeSourcePhrases[bsIncludeSourcePhraseCount - 1], "~");
}
