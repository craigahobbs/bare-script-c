/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The regular expression engine unit tests
 */

#include <stdio.h>
#include <string.h>

#include "test.h"


/* Match a pattern against a subject, returning the matched text or NULL */
static BSValue bsTestMatch(const char *pattern, const char *subject, unsigned flags)
{
    char error[BS_REGEX_ERROR_MAX];
    BSValue regex = bsRegexNew(pattern, strlen(pattern), flags, error, sizeof(error));
    if (regex.type == BS_NULL) {
        bsTestFail(__FILE__, __LINE__, "bsRegexNew(%s) failed: %s", pattern, error);
    }
    BSValue string = bsStringNew(subject);
    BSRegexSubject subjectCodes;
    bsRegexSubjectInit(&subjectCodes, string);
    BSRegexMatch match;
    BSValue result = bsNull();
    if (bsRegexSearch(regex, &subjectCodes, 0, &match)) {
        size_t begin = bsStringOffset(string, match.begin);
        size_t end = bsStringOffset(string, match.end);
        result = bsStringNewSize(bsStringData(string) + begin, end - begin);
    }
    bsRegexSubjectFree(&subjectCodes);
    bsRelease(string);
    bsRelease(regex);
    return result;
}


/* Match a pattern and return the match's capture groups as a JSON-comparable array */
static BSValue bsTestGroups(const char *pattern, const char *subject, unsigned flags)
{
    BSValue regex = bsRegexNew(pattern, strlen(pattern), flags, NULL, 0);
    BSValue string = bsStringNew(subject);
    BSRegexSubject subjectCodes;
    bsRegexSubjectInit(&subjectCodes, string);
    BSRegexMatch match;
    BSValue result = bsNull();
    if (bsRegexSearch(regex, &subjectCodes, 0, &match)) {
        result = bsArrayNew();
        for (size_t ix = 0; ix < match.groupCount; ix++) {
            if (match.matched[ix]) {
                size_t begin = bsStringOffset(string, match.groups[ix].begin);
                size_t end = bsStringOffset(string, match.groups[ix].end);
                bsArrayPush(result, bsStringNewSize(bsStringData(string) + begin, end - begin));
            } else {
                bsArrayPush(result, bsNull());
            }
        }
    }
    bsRegexSubjectFree(&subjectCodes);
    bsRelease(string);
    bsRelease(regex);
    return result;
}


/* Assert a pattern fails to compile with the given error */
static void bsTestRegexError(const char *pattern, const char *expectedError)
{
    char error[BS_REGEX_ERROR_MAX];
    BSValue regex = bsRegexNew(pattern, strlen(pattern), 0, error, sizeof(error));
    if (!bsTestStringEqual(error, expectedError)) {
        bsTestFail(__FILE__, __LINE__, "bsRegexNew(%s)\n    actual:   %s\n    expected: %s", pattern,
                   error, expectedError);
    } else {
        bsTestPass();
    }
    bsRelease(regex);
}


TEST(regex_literals)
{
    ASSERT_VALUE_STRING(bsTestMatch("abc", "xxabcyy", 0), "abc");
    ASSERT_VALUE_STRING(bsTestMatch("abc", "xxabyy", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("", "abc", 0), "");
    ASSERT_VALUE_STRING(bsTestMatch("a", "", 0), "null");
}


TEST(regex_dot_and_classes)
{
    ASSERT_VALUE_STRING(bsTestMatch("a.c", "abc", 0), "abc");
    ASSERT_VALUE_STRING(bsTestMatch("a.c", "a\nc", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("a.c", "a\nc", BS_REGEX_DOTALL), "a\nc");
    ASSERT_VALUE_STRING(bsTestMatch("[abc]+", "xxbcayy", 0), "bca");
    ASSERT_VALUE_STRING(bsTestMatch("[^abc]+", "abxyzc", 0), "xyz");
    ASSERT_VALUE_STRING(bsTestMatch("[a-c]+", "xbcay", 0), "bca");
    ASSERT_VALUE_STRING(bsTestMatch("[]", "a", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("[]]", "]", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("[a-]", "-", 0), "-");
    ASSERT_VALUE_STRING(bsTestMatch("[\\]]", "]", 0), "]");
    ASSERT_VALUE_STRING(bsTestMatch("[\\b]", "\b", 0), "\b");
    ASSERT_VALUE_STRING(bsTestMatch("\\d+", "ab123c", 0), "123");
    ASSERT_VALUE_STRING(bsTestMatch("\\D+", "12ab34", 0), "ab");
    ASSERT_VALUE_STRING(bsTestMatch("\\w+", " ab_1 ", 0), "ab_1");
    ASSERT_VALUE_STRING(bsTestMatch("\\W+", "ab  cd", 0), "  ");
    ASSERT_VALUE_STRING(bsTestMatch("\\s+", "ab  cd", 0), "  ");
    ASSERT_VALUE_STRING(bsTestMatch("\\S+", "  ab  ", 0), "ab");
    ASSERT_VALUE_STRING(bsTestMatch("[\\d]+", "ab12", 0), "12");
    ASSERT_VALUE_STRING(bsTestMatch("[\\s\\S]+", "a\nb", 0), "a\nb");
    ASSERT_VALUE_STRING(bsTestMatch("[^\\d]+", "12ab", 0), "ab");
    ASSERT_VALUE_STRING(bsTestMatch("[\\D]+", "12ab", 0), "ab");
    ASSERT_VALUE_STRING(bsTestMatch("[\\W]+", "ab  ", 0), "  ");

    /* Unicode literals and classes */
    ASSERT_VALUE_STRING(bsTestMatch("\xc3\xa9+", "a\xc3\xa9\xc3\xa9z", 0), "\xc3\xa9\xc3\xa9");
    ASSERT_VALUE_STRING(bsTestMatch("[\xc3\xa9-\xc3\xaa]+", "a\xc3\xa9z", 0), "\xc3\xa9");

    /* Code points of 256 or more go through the class's range walk, not the ASCII bitmap */
    ASSERT_VALUE_STRING(bsTestMatch("\\s", "\xe2\x80\xa8", 0), "\xe2\x80\xa8"); /* U+2028 */
    ASSERT_VALUE_STRING(bsTestMatch("[\\u2600]", "x\xe2\x98\x80y", 0), "\xe2\x98\x80");
    ASSERT_VALUE_STRING(bsTestMatch("[^\\u2600]", "\xe2\x98\x80" "a", 0), "a");
    ASSERT_VALUE_STRING(bsTestMatch("[^\\s]", "\xe2\x98\x80", 0), "\xe2\x98\x80");
}


TEST(regex_escapes)
{
    ASSERT_VALUE_STRING(bsTestMatch("a\\nb", "a\nb", 0), "a\nb");
    ASSERT_VALUE_STRING(bsTestMatch("a\\rb", "a\rb", 0), "a\rb");
    ASSERT_VALUE_STRING(bsTestMatch("a\\tb", "a\tb", 0), "a\tb");
    ASSERT_VALUE_STRING(bsTestMatch("a\\fb", "a\fb", 0), "a\fb");
    ASSERT_VALUE_STRING(bsTestMatch("a\\vb", "a\vb", 0), "a\vb");
    ASSERT_VALUE_STRING(bsTestMatch("a\\0", "ab", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("\\x41", "xAy", 0), "A");
    ASSERT_VALUE_STRING(bsTestMatch("\\u0041", "xAy", 0), "A");
    ASSERT_VALUE_STRING(bsTestMatch("\\u00e9", "x\xc3\xa9y", 0), "\xc3\xa9");
    ASSERT_VALUE_STRING(bsTestMatch("a\\.c", "abc a.c", 0), "a.c");
    ASSERT_VALUE_STRING(bsTestMatch("\\$", "a$b", 0), "$");
}


TEST(regex_anchors)
{
    ASSERT_VALUE_STRING(bsTestMatch("^ab", "abc", 0), "ab");
    ASSERT_VALUE_STRING(bsTestMatch("^bc", "abc", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("bc$", "abc", 0), "bc");
    ASSERT_VALUE_STRING(bsTestMatch("ab$", "abc", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("^b", "a\nb", BS_REGEX_MULTILINE), "b");
    ASSERT_VALUE_STRING(bsTestMatch("a$", "a\nb", BS_REGEX_MULTILINE), "a");
    ASSERT_VALUE_STRING(bsTestMatch("\\bword\\b", "a word here", 0), "word");
    ASSERT_VALUE_STRING(bsTestMatch("\\bword\\b", "sword", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("\\Bord", "sword", 0), "ord");
    ASSERT_VALUE_STRING(bsTestMatch("\\Bord", "a ord", 0), "null");
}


TEST(regex_quantifiers)
{
    ASSERT_VALUE_STRING(bsTestMatch("a*", "aaa", 0), "aaa");
    ASSERT_VALUE_STRING(bsTestMatch("a*", "bbb", 0), "");
    ASSERT_VALUE_STRING(bsTestMatch("a+", "xaaay", 0), "aaa");
    ASSERT_VALUE_STRING(bsTestMatch("a+", "bbb", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("ab?c", "ac", 0), "ac");
    ASSERT_VALUE_STRING(bsTestMatch("ab?c", "abc", 0), "abc");
    ASSERT_VALUE_STRING(bsTestMatch("a{2}", "aaa", 0), "aa");
    ASSERT_VALUE_STRING(bsTestMatch("a{2,}", "aaaa", 0), "aaaa");
    ASSERT_VALUE_STRING(bsTestMatch("a{2,3}", "aaaa", 0), "aaa");
    ASSERT_VALUE_STRING(bsTestMatch("a{4}", "aaa", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("a{0}b", "b", 0), "b");
    ASSERT_VALUE_STRING(bsTestMatch("a+?", "aaa", 0), "a");
    ASSERT_VALUE_STRING(bsTestMatch("a*?b", "aaab", 0), "aaab");
    ASSERT_VALUE_STRING(bsTestMatch("a{2,3}?", "aaaa", 0), "aa");
    ASSERT_VALUE_STRING(bsTestMatch("a??b", "ab", 0), "ab");

    /* A "{" that is not a quantifier is a literal */
    ASSERT_VALUE_STRING(bsTestMatch("a{b", "a{b", 0), "a{b");
    ASSERT_VALUE_STRING(bsTestMatch("a{2", "a{2", 0), "a{2");
    ASSERT_VALUE_STRING(bsTestMatch("a{,2}", "a{,2}", 0), "a{,2}");

    /* Quantified groups and alternations backtrack */
    /* Capture writes overflow the inline undo trail and then its first heap block */
    ASSERT_VALUE_STRING(bsTestMatch("(a){40}",
                                   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0),
                       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    ASSERT_VALUE_STRING(bsTestMatch("(a){70}",
                                   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0),
                       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

    ASSERT_VALUE_STRING(bsTestMatch("(ab)+", "xababy", 0), "abab");
    ASSERT_VALUE_STRING(bsTestMatch("(ab)+?", "xababy", 0), "ab");
    ASSERT_VALUE_STRING(bsTestMatch("(?:a|b)+", "xabay", 0), "aba");
    ASSERT_VALUE_STRING(bsTestMatch("(a|b){2,3}", "xabay", 0), "aba");
    ASSERT_VALUE_STRING(bsTestMatch("(a*)*b", "aaab", 0), "aaab");
    ASSERT_VALUE_STRING(bsTestMatch("(?:)*a", "a", 0), "a");
    ASSERT_VALUE_STRING(bsTestMatch("(?:a?)*b", "b", 0), "b");
    ASSERT_VALUE_STRING(bsTestMatch("(?:a|)+b", "aab", 0), "aab");
}


TEST(regex_alternation_and_groups)
{
    ASSERT_VALUE_STRING(bsTestMatch("a|b", "zb", 0), "b");
    ASSERT_VALUE_STRING(bsTestMatch("abc|abd", "xabd", 0), "abd");
    ASSERT_VALUE_STRING(bsTestMatch("a||b", "a", 0), "a");
    ASSERT_VALUE_STRING(bsTestMatch("(a)(b)", "ab", 0), "ab");
    ASSERT_VALUE(bsTestGroups("(a)(b)", "ab", 0), "[\"ab\",\"a\",\"b\"]");
    ASSERT_VALUE(bsTestGroups("(a)|(b)", "b", 0), "[\"b\",null,\"b\"]");
    ASSERT_VALUE(bsTestGroups("(?:a)(b)", "ab", 0), "[\"ab\",\"b\"]");
    ASSERT_VALUE(bsTestGroups("(?<name>a)(b)", "ab", 0), "[\"ab\",\"a\",\"b\"]");
    ASSERT_VALUE(bsTestGroups("((a)(b))", "ab", 0), "[\"ab\",\"ab\",\"a\",\"b\"]");
    ASSERT_VALUE(bsTestGroups("(a)?b", "b", 0), "[\"b\",null]");

    /* Group names */
    BSValue regex = bsRegexNew("(?<year>[0-9]{4})-([0-9]{2})", 28, 0, NULL, 0);
    ASSERT_INT_EQ(bsRegexGroupCount(regex), 3);
    ASSERT_STR_EQ(bsRegexGroupName(regex, 1), "year");
    ASSERT_NULL(bsRegexGroupName(regex, 2));
    ASSERT_NULL(bsRegexGroupName(regex, 9));
    ASSERT_STR_EQ(bsRegexPattern(regex), "(?<year>[0-9]{4})-([0-9]{2})");
    ASSERT_INT_EQ(bsRegexFlags(regex), 0);
    bsRelease(regex);
}


TEST(regex_backreferences)
{
    ASSERT_VALUE_STRING(bsTestMatch("(\\w)\\1", "xaay", 0), "aa");
    ASSERT_VALUE_STRING(bsTestMatch("(\\w)\\1", "xaby", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("(?<w>\\w)\\k<w>", "xaay", 0), "aa");
    ASSERT_VALUE_STRING(bsTestMatch("(a)?\\1b", "b", 0), "b");
    ASSERT_VALUE_STRING(bsTestMatch("(abc)\\1", "abcab", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("(A)\\1", "aa", BS_REGEX_IGNORECASE), "aa");
    ASSERT_VALUE_STRING(bsTestMatch("\\1(a)", "a", 0), "a");
}


TEST(regex_lookaround)
{
    ASSERT_VALUE_STRING(bsTestMatch("a(?=b)", "ab", 0), "a");
    ASSERT_VALUE_STRING(bsTestMatch("a(?=b)", "ac", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("a(?!b)", "ab ac", 0), "a");
    ASSERT_VALUE_STRING(bsTestMatch("(?<=a)b", "ab", 0), "b");
    ASSERT_VALUE_STRING(bsTestMatch("(?<=a)b", "xb", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("(?<!a)b", "xb", 0), "b");
    ASSERT_VALUE_STRING(bsTestMatch("(?<!a)b", "ab", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("(?<!ab)c", "xc", 0), "c");
    ASSERT_VALUE_STRING(bsTestMatch("(?<!ab)c", "abc", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("(?<=ab+)c", "abbbc", 0), "c");
    ASSERT_VALUE_STRING(bsTestMatch("(?<=^)a", "ab", 0), "a");
    ASSERT_VALUE_STRING(bsTestMatch("(?<=a|xy)b", "xyb", 0), "b");
    ASSERT_VALUE_STRING(bsTestMatch("(?<=(a))b", "ab", 0), "b");

    /* A failing continuation after a successful lookaround restores the captures */
    ASSERT_VALUE(bsTestGroups("(?:(?=(a))ab|(?=(a))ac)", "ac", 0), "[\"ac\",null,\"a\"]");
    ASSERT_VALUE(bsTestGroups("(?:(?<=(a))b|(?<=(a))c)", "ac", 0), "[\"c\",null,\"a\"]");
    ASSERT_VALUE_STRING(bsTestMatch("(?!(a))b", "b", 0), "b");
}


TEST(regex_ignorecase)
{
    ASSERT_VALUE_STRING(bsTestMatch("abc", "xABCy", BS_REGEX_IGNORECASE), "ABC");
    ASSERT_VALUE_STRING(bsTestMatch("ABC", "xabcy", BS_REGEX_IGNORECASE), "abc");
    ASSERT_VALUE_STRING(bsTestMatch("[a-c]+", "xABCy", BS_REGEX_IGNORECASE), "ABC");
    ASSERT_VALUE_STRING(bsTestMatch("[^a-c]+", "ABxy", BS_REGEX_IGNORECASE), "xy");
    ASSERT_VALUE_STRING(bsTestMatch("[0-9]+", "AB12", BS_REGEX_IGNORECASE), "12");
}


TEST(regex_search_start)
{
    BSValue regex = bsRegexNew("a", 1, 0, NULL, 0);
    BSValue string = bsStringNew("aXa");
    BSRegexSubject subject;
    bsRegexSubjectInit(&subject, string);
    BSRegexMatch match;
    ASSERT_TRUE(bsRegexSearch(regex, &subject, 0, &match));
    ASSERT_INT_EQ(match.begin, 0);
    ASSERT_TRUE(bsRegexSearch(regex, &subject, 1, &match));
    ASSERT_INT_EQ(match.begin, 2);
    ASSERT_FALSE(bsRegexSearch(regex, &subject, 3, &match));
    bsRegexSubjectFree(&subject);
    bsRelease(string);
    bsRelease(regex);
}


TEST(regex_subject_long)
{
    /* A subject longer than the inline buffer allocates */
    BSStringBuilder sb;
    bsSBInit(&sb);
    for (int ix = 0; ix < 200; ix++) {
        bsSBAppendChar(&sb, 'x');
    }
    bsSBAppendString(&sb, "target");
    BSValue string = bsSBToValue(&sb);
    BSRegexSubject subject;
    bsRegexSubjectInit(&subject, string);
    ASSERT_NOT_NULL(subject.owned);
    ASSERT_INT_EQ(subject.length, 206);
    BSValue regex = bsRegexNew("target", 6, 0, NULL, 0);
    BSRegexMatch match;
    ASSERT_TRUE(bsRegexSearch(regex, &subject, 0, &match));
    ASSERT_INT_EQ(match.begin, 200);
    bsRegexSubjectFree(&subject);
    bsRelease(string);
    bsRelease(regex);
}


TEST(regex_compile_errors)
{
    bsTestRegexError("(", "missing ), unterminated subpattern at position 0");
    bsTestRegexError(")", "unbalanced parenthesis at position 0");
    bsTestRegexError("(?", "unexpected end of pattern at position 2");
    bsTestRegexError("(?P<a>b)", "unknown extension ?P at position 1");
    bsTestRegexError("(?<>a)", "unknown extension ?<> at position 1");
    bsTestRegexError("(?<a-b>c)", "unknown extension ?<a at position 1");
    bsTestRegexError("(?<a", "unknown extension ?<a at position 1");
    bsTestRegexError("(?<", "unexpected end of pattern at position 3");
    bsTestRegexError("[a", "unterminated character set at position 0");
    bsTestRegexError("[a\\", "bad escape (end of pattern) at position 2");
    bsTestRegexError("[a-\\", "bad escape (end of pattern) at position 3");
    bsTestRegexError("[c-a]", "bad character range c-a at position 1");
    bsTestRegexError("[a-\\d]", "bad character range a-\\d at position 1");
    bsTestRegexError("a{3,2}", "min repeat greater than max repeat at position 2");
    bsTestRegexError("\\", "bad escape (end of pattern) at position 0");
    bsTestRegexError("\\x4", "incomplete escape \\x4 at position 0");
    bsTestRegexError("\\u04", "incomplete escape \\u04 at position 0");
    bsTestRegexError("[\\x4]", "incomplete escape \\x4 at position 1");
    bsTestRegexError("\\k<none>", "unknown group name 'none' at position 4");
    bsTestRegexError("\\k<", "bad escape \\k at position 0");
    bsTestRegexError("\\k", "bad escape \\k at position 0");
    bsTestRegexError("\\kx", "bad escape \\k at position 0");
    bsTestRegexError("*", "nothing to repeat at position 0");
    bsTestRegexError("{2}", "nothing to repeat at position 0");
    bsTestRegexError("(|*)", "nothing to repeat at position 2");
    bsTestRegexError("^*", "nothing to repeat at position 1");
    bsTestRegexError("$?", "nothing to repeat at position 1");
    bsTestRegexError("\\b+", "nothing to repeat at position 2");
    bsTestRegexError("\\B{2}", "nothing to repeat at position 2");
    bsTestRegexError("(?\\d)", "unknown extension ?\\d at position 1");
    bsTestRegexError("(?<\\w>a)", "unknown extension ?<\\w at position 1");
    bsTestRegexError("a**", "multiple repeat at position 2");
    bsTestRegexError("a{2}{3}", "multiple repeat at position 4");
    bsTestRegexError("a*?*", "multiple repeat at position 3");
    bsTestRegexError("\\999", "invalid group reference 999 at position 1");

    /* Too many capture groups */
    BSStringBuilder sb;
    bsSBInit(&sb);
    for (int ix = 0; ix < 130; ix++) {
        bsSBAppendString(&sb, "(a)");
    }
    BSValue pattern = bsSBToValue(&sb);
    char error[BS_REGEX_ERROR_MAX];
    bsRelease(bsRegexNew(bsStringData(pattern), bsStringSize(pattern), 0, error, sizeof(error)));
    ASSERT_STR_EQ(error, "sorry, but this version only supports 127 groups at position 381");
    bsRelease(pattern);

    /* The error argument is optional */
    bsRelease(bsRegexNew("(", 1, 0, NULL, 0));
    bsRelease(bsRegexNew("a", 1, 0, NULL, 0));
}


TEST(regex_depth_limit)
{
    /* A pathological pattern gives up on its step budget rather than hanging */
    BSStringBuilder sb;
    bsSBInit(&sb);
    for (int ix = 0; ix < 30; ix++) {
        bsSBAppendChar(&sb, 'a');
    }
    BSValue subject = bsSBToValue(&sb);
    BSValue regex = bsRegexNew("(a|aa)+$b", 9, 0, NULL, 0);
    BSRegexSubject subjectCodes;
    bsRegexSubjectInit(&subjectCodes, subject);
    BSRegexMatch match;
    ASSERT_FALSE(bsRegexSearch(regex, &subjectCodes, 0, &match));
    bsRegexSubjectFree(&subjectCodes);
    bsRelease(subject);
    bsRelease(regex);

    /* A long simple repeat matches iteratively, without recursion */
    bsSBInit(&sb);
    for (int ix = 0; ix < 20000; ix++) {
        bsSBAppendChar(&sb, 'x');
    }
    bsSBAppendChar(&sb, 'y');
    BSValue longSubject = bsSBToValue(&sb);
    BSValue longRegex = bsRegexNew("x*y", 3, 0, NULL, 0);
    bsRegexSubjectInit(&subjectCodes, longSubject);
    ASSERT_TRUE(bsRegexSearch(longRegex, &subjectCodes, 0, &match));
    ASSERT_INT_EQ(match.end, 20001);
    bsRegexSubjectFree(&subjectCodes);
    bsRelease(longSubject);
    bsRelease(longRegex);
}


TEST(regex_escape)
{
    ASSERT_VALUE_STRING(bsRegexEscape(bsStringNew("a.b*c[d]")), "a\\.b\\*c\\[d\\]");
    ASSERT_VALUE_STRING(bsRegexEscape(bsStringNew("abc")), "abc");
    ASSERT_VALUE_STRING(bsRegexEscape(bsStringNew("^$\\.+?()|{}[]*")), "\\^\\$\\\\\\.\\+\\?\\(\\)\\|\\{\\}\\[\\]\\*");
    ASSERT_VALUE_STRING(bsRegexEscape(bsStringNew("\xc3\xa9")), "\xc3\xa9");
}


TEST(regex_coverage_gaps)
{
    /* Case-insensitive matching of a non-letter falls through the case swap */
    ASSERT_VALUE_STRING(bsTestMatch("[a-c]+", "12ab", BS_REGEX_IGNORECASE), "ab");
    ASSERT_VALUE_STRING(bsTestMatch("[^0-9]+", "12ab", BS_REGEX_IGNORECASE), "ab");

    /* Upper-case hex escapes */
    ASSERT_VALUE_STRING(bsTestMatch("\\x4A", "xJy", 0), "J");
    ASSERT_VALUE_STRING(bsTestMatch("\\u004A", "xJy", 0), "J");
    bsTestRegexError("\\x4G", "incomplete escape \\x4 at position 0");
    bsTestRegexError("[a\\x4G]", "incomplete escape \\x4 at position 2");

    /* A character class range with an escaped high bound */
    ASSERT_VALUE_STRING(bsTestMatch("[a-\\x63]+", "xabcy", 0), "abc");
    ASSERT_VALUE_STRING(bsTestMatch("[\\x61-c]+", "xabcy", 0), "abc");

    /* A named group whose name is used by a backreference */
    ASSERT_VALUE_STRING(bsTestMatch("(?<a>x)(?<b>y)\\k<b>", "xyy", 0), "xyy");

    /* A "{" quantifier that is not one, after a group */
    ASSERT_VALUE_STRING(bsTestMatch("(a){x", "a{x", 0), "a{x");
    ASSERT_VALUE_STRING(bsTestMatch("a{1", "a{1", 0), "a{1");

    /* Lookbehind length bounds over repeats, alternations, and backreferences */
    ASSERT_VALUE_STRING(bsTestMatch("(?<=a{2})b", "aab", 0), "b");
    ASSERT_VALUE_STRING(bsTestMatch("(?<=a{2,3})b", "aaab", 0), "b");
    ASSERT_VALUE_STRING(bsTestMatch("(a)(?<=\\1)b", "ab", 0), "ab");
    ASSERT_VALUE_STRING(bsTestMatch("(a)(?<=\\1x)b", "ab", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("(?<=(?:ab)*c)d", "ababcd", 0), "d");
    ASSERT_VALUE_STRING(bsTestMatch("(?<=^a*)b", "aab", 0), "b");

    /* Lazy repeats over non-simple bodies */
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab)+?c", "ababc", 0), "ababc");
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab)*?c", "ababc", 0), "ababc");
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab)*?", "abab", 0), "");
    ASSERT_VALUE_STRING(bsTestMatch("(?:a|b){2,}?c", "abc", 0), "abc");
    ASSERT_VALUE_STRING(bsTestMatch("x(?:ab)*?y", "xy", 0), "xy");

    /* A lazy simple repeat that runs out of input */
    ASSERT_VALUE_STRING(bsTestMatch("a+?b", "aaa", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("a{2,3}?b", "aa", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("a{4,}?", "aa", 0), "null");

    /* Multi-line anchors */
    ASSERT_VALUE_STRING(bsTestMatch("^$", "", BS_REGEX_MULTILINE), "");
    ASSERT_VALUE_STRING(bsTestMatch("a$", "ab", BS_REGEX_MULTILINE), "null");
    ASSERT_VALUE_STRING(bsTestMatch("^b", "ab", BS_REGEX_MULTILINE), "null");
}


TEST(regex_coverage_gaps2)
{
    /* An invalid escape as a character class range's high bound */
    bsTestRegexError("[a-\\x4G]", "incomplete escape \\x4 at position 3");

    /* An invalid sub-pattern inside a group */
    bsTestRegexError("(\\x4G)", "incomplete escape \\x4 at position 1");
    bsTestRegexError("(?:[c-a])", "bad character range c-a at position 4");

    /* A lookbehind whose length bound saturates on an unbounded body */
    ASSERT_VALUE_STRING(bsTestMatch("(?<=\\1{2})b", "ab", 0), "b");
    ASSERT_VALUE_STRING(bsTestMatch("(?<=\\1{2,4})b", "ab", 0), "b");

    /* Lazy simple repeats that cannot meet their minimum */
    ASSERT_VALUE_STRING(bsTestMatch("a{3,}?b", "aab", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("x{2,}?", "", 0), "null");

    /* A lazy non-simple repeat with a zero minimum tries its continuation first */
    ASSERT_VALUE_STRING(bsTestMatch("x(?:ab)*?y", "xaby", 0), "xaby");
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab)??c", "abc", 0), "abc");
}


TEST(regex_final_coverage)
{
    /* A greedy simple repeat that matches zero times and whose continuation fails */
    ASSERT_VALUE_STRING(bsTestMatch("a*b", "c", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("a*$", "b", 0), "");

    /* A lazy simple repeat whose minimum cannot be met */
    ASSERT_VALUE_STRING(bsTestMatch("a{3,}?", "aa", 0), "null");

    /* A non-simple repeat with a zero maximum matches nothing */
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab){0}c", "c", 0), "c");
    ASSERT_VALUE_STRING(bsTestMatch("(a){0}b", "b", 0), "b");
}


TEST(regex_first_set)
{
    /* The search skips positions whose code point cannot begin a match */
    ASSERT_VALUE_STRING(bsTestMatch("(?:abc|xyz)", "12345xyz", 0), "xyz");
    ASSERT_VALUE_STRING(bsTestMatch("[bd]x", "aaaabx", 0), "bx");
    ASSERT_VALUE_STRING(bsTestMatch("(a)b", "zzzab", 0), "ab");
    ASSERT_VALUE_STRING(bsTestMatch("a*bc", "zzzbc", 0), "bc");
    ASSERT_VALUE_STRING(bsTestMatch("(?<!x)qr", "aaqr", 0), "qr");
    ASSERT_VALUE_STRING(bsTestMatch("\\bqr", "aa qr", 0), "qr");
    ASSERT_VALUE_STRING(bsTestMatch("(?:abc|xyz)", "nothing here", 0), "null");

    /* A first set that includes non-ASCII code points */
    ASSERT_VALUE_STRING(bsTestMatch("\xc3\xa9x", "aaa\xc3\xa9x", 0), "\xc3\xa9x");

    /* A first code point above the first set's table, and a class range that reaches past it */
    ASSERT_VALUE_STRING(bsTestMatch("\xe6\xbc\xa2x", "aa\xe6\xbc\xa2x", 0), "\xe6\xbc\xa2x");
    ASSERT_VALUE_STRING(bsTestMatch("[\xe6\xbc\xa2-\xe6\xbc\xa5]x", "aa\xe6\xbc\xa3x", 0),
                        "\xe6\xbc\xa3x");
    ASSERT_VALUE_STRING(bsTestMatch("[a-\xe6\xbc\xa2]y", "  \xe6\xbc\xa2y", 0), "\xe6\xbc\xa2y");
    ASSERT_VALUE_STRING(bsTestMatch("[\xe6\xbc\xa2-\xe6\xbc\xa5]x", "aabx", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("[\xc3\xa9-\xc3\xaa]x", "aa\xc3\xaax", 0), "\xc3\xaax");
    ASSERT_VALUE_STRING(bsTestMatch("[a-\xc3\xaa]+", "\xc3\xa9z", 0), "\xc3\xa9z");

    /* Case-insensitive matching widens the first set */
    ASSERT_VALUE_STRING(bsTestMatch("abc", "zzzABC", BS_REGEX_IGNORECASE), "ABC");
    ASSERT_VALUE_STRING(bsTestMatch("[a-c]x", "zzzBx", BS_REGEX_IGNORECASE), "Bx");

    /* Patterns the first set cannot constrain still match */
    ASSERT_VALUE_STRING(bsTestMatch("a*", "zzz", 0), "");
    ASSERT_VALUE_STRING(bsTestMatch(".c", "zzzac", 0), "ac");
    ASSERT_VALUE_STRING(bsTestMatch("[^q]c", "zzzac", 0), "ac");
    ASSERT_VALUE_STRING(bsTestMatch("\\wc", "  ac", 0), "ac");
    ASSERT_VALUE_STRING(bsTestMatch("(a)?\\1c", "zzc", 0), "c");
    ASSERT_VALUE_STRING(bsTestMatch("(?:)x", "zzx", 0), "x");
}
