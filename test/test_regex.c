/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The regular expression engine unit tests
 */

#include <string.h>

#include "test.h"
#include "../src/internal.h"


/* Match a pattern and return the match's capture groups as a JSON-comparable array, or null */
static BSValue bsTestGroups(const char *pattern, const char *subject, unsigned flags)
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


/* Match a pattern against a subject, returning the matched text - group 0 - or null */
static BSValue bsTestMatch(const char *pattern, const char *subject, unsigned flags)
{
    BSValue groups = bsTestGroups(pattern, subject, flags);
    BSValue result = groups.type == BS_ARRAY ? bsRetain(bsArrayGet(groups, 0)) : bsNull();
    bsRelease(groups);
    return result;
}


/* Assert a pattern fails to compile with the given error */
static void bsTestRegexError(const char *pattern, const char *expectedError)
{
    char error[BS_REGEX_ERROR_MAX];
    BSValue regex = bsRegexNew(pattern, strlen(pattern), 0, error, sizeof(error));
    bsTestAssertEqual(__FILE__, __LINE__, pattern, error, expectedError);
    bsRelease(regex);
}


TEST(regex_literals)
{
    ASSERT_VALUE_STRING(bsTestMatch("abc", "xxabcyy", 0), "abc");
    ASSERT_VALUE_STRING(bsTestMatch("abc", "xxabyy", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("", "abc", 0), "");
    ASSERT_VALUE_STRING(bsTestMatch("a", "", 0), "null");

    /* An unnamed pattern stores no group-name array */
    BSValue regex = bsRegexNew("abc", 3, 0, NULL, 0);
    ASSERT_INT_EQ(bsRegexGroupNameValue(regex, 0).type, BS_NULL);
    bsRelease(regex);
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

    /* Code points past ASCII go through the class's range walk, not the ASCII bitmap */
    ASSERT_VALUE_STRING(bsTestMatch("\\s", "\xe2\x80\xa8", 0), "\xe2\x80\xa8"); /* U+2028 */
    ASSERT_VALUE_STRING(bsTestMatch("\\s", "\xe2\x80\x83", 0), "\xe2\x80\x83"); /* U+2003 */
    ASSERT_VALUE_STRING(bsTestMatch("\\s", "\xe3\x80\x80", 0), "\xe3\x80\x80"); /* U+3000 */
    ASSERT_VALUE_STRING(bsTestMatch("\\s", "\xef\xbb\xbf", 0), "\xef\xbb\xbf"); /* U+FEFF */
    ASSERT_VALUE_STRING(bsTestMatch("\\s", "\xc2\x85", 0), "null"); /* U+0085 */
    ASSERT_VALUE_STRING(bsTestMatch("[\\u2600]", "x\xe2\x98\x80y", 0), "\xe2\x98\x80");
    ASSERT_VALUE_STRING(bsTestMatch("[^\\u2600]", "\xe2\x98\x80" "a", 0), "a");
    ASSERT_VALUE_STRING(bsTestMatch("[^\\s]", "\xe2\x98\x80", 0), "\xe2\x98\x80");

    /* Unicode subjects take the code-point matcher, not the ASCII byte path */
    ASSERT_VALUE_STRING(bsTestMatch(".", "\xc3\xa9", 0), "\xc3\xa9");
    ASSERT_VALUE_STRING(bsTestMatch(".", "\xe2\x80\xa8", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch(".", "\xe2\x80\xa8", BS_REGEX_DOTALL), "\xe2\x80\xa8");
    ASSERT_VALUE_STRING(bsTestMatch("ab", "\xc3\xa9" "ab", 0), "ab");
    ASSERT_VALUE_STRING(bsTestMatch(".+", "a" "\xc3\xa9" "z", 0), "a" "\xc3\xa9" "z");
    ASSERT_VALUE_STRING(bsTestMatch(".+?", "a" "\xc3\xa9" "z", 0), "a");
}


TEST(regex_escapes)
{
    ASSERT_VALUE_STRING(bsTestMatch("a\\nb", "a\nb", 0), "a\nb");
    ASSERT_VALUE_STRING(bsTestMatch("a\\rb", "a\rb", 0), "a\rb");
    ASSERT_VALUE_STRING(bsTestMatch("a\\tb", "a\tb", 0), "a\tb");
    ASSERT_VALUE_STRING(bsTestMatch("a\\fb", "a\fb", 0), "a\fb");
    ASSERT_VALUE_STRING(bsTestMatch("a\\vb", "a\vb", 0), "a\vb");
    ASSERT_VALUE_STRING(bsTestMatch("a\\0", "ab", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("\\012", "a\nb", 0), "\n");
    ASSERT_VALUE_STRING(bsTestMatch("\\0777", "x?7", 0), "?7");
    ASSERT_VALUE_STRING(bsTestMatch("[\\1\\123]+", "x\x01S", 0), "\x01S");
    ASSERT_VALUE_STRING(bsTestMatch("[\\477]", "x'", 0), "'");
    ASSERT_VALUE_STRING(bsTestMatch("\\120\\1200", "xPP0", 0), "PP0");
    ASSERT_VALUE_STRING(bsTestMatch("\\cA\\cj", "x\x01\n", 0), "\x01\n");
    ASSERT_VALUE_STRING(bsTestMatch("[\\cA]", "x\x01", 0), "\x01");
    ASSERT_VALUE_STRING(bsTestMatch("\\c1", "a\\c1", 0), "\\c1");
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

    /* Unicode subjects take the code-point BOL / EOL / word-boundary path */
    ASSERT_VALUE_STRING(bsTestMatch("^\xc3\xa9", "\xc3\xa9" "abc", 0), "\xc3\xa9");
    ASSERT_VALUE_STRING(bsTestMatch("^b", "\xc3\xa9" "a\nb", BS_REGEX_MULTILINE), "b");
    ASSERT_VALUE_STRING(bsTestMatch("a$", "\xc3\xa9" "a\nb", BS_REGEX_MULTILINE), "a");

    /* Every line terminator - CR, U+2028, U+2029 - bounds a multi-line anchor; "$" alone is the end only */
    ASSERT_VALUE_STRING(bsTestMatch("^b", "a\rb", BS_REGEX_MULTILINE), "b");
    ASSERT_VALUE_STRING(bsTestMatch("^b", "a\xe2\x80\xa8" "b", BS_REGEX_MULTILINE), "b");
    ASSERT_VALUE_STRING(bsTestMatch("a$", "a\xe2\x80\xa9" "b", BS_REGEX_MULTILINE), "a");
    ASSERT_VALUE_STRING(bsTestMatch("^b", "a\rb", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("a$", "a\n", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("c$", "\xc3\xa9" "abc", 0), "c");
    ASSERT_VALUE_STRING(bsTestMatch("\\bword\\b", "\xc3\xa9" " word ", 0), "word");
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

    /* Counts saturate rather than overflow */
    ASSERT_VALUE_STRING(bsTestMatch("a{4294967296}", "aaa", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("a{1,99999999999}", "aaa", 0), "aaa");
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

    /* A failed attempt leaves no capture behind for the next start position - a stale flag and span once
       made a backreference read past the subject */
    ASSERT_VALUE(bsTestGroups("(?:(a)|b)\\1", "ab", 0), "[\"b\",null]");
    ASSERT_VALUE(bsTestGroups("(a\\1)", "xaa", 0), "[\"a\",\"a\"]");
    ASSERT_VALUE(bsTestGroups("(a)??c", "abc", 0), "[\"c\",null]");

    /* Every iteration of a repeated group begins with its captures unset, and an iteration past the
       required ones that matches nothing fails, leaving the repetition as it was - JavaScript's rules */
    ASSERT_VALUE(bsTestGroups("((a)|b)*", "ab", 0), "[\"ab\",\"b\",null]");
    ASSERT_VALUE(bsTestGroups("((a)|b)*", "ba", 0), "[\"ba\",\"a\",\"a\"]");
    ASSERT_VALUE(bsTestGroups("(a*)*", "a", 0), "[\"a\",\"a\"]");
    ASSERT_VALUE(bsTestGroups("(a*)*", "b", 0), "[\"\",null]");
    ASSERT_VALUE(bsTestGroups("(a*){2,}", "", 0), "[\"\",\"\"]");
    ASSERT_VALUE(bsTestGroups("(?:(a)|(b))+", "ab", 0), "[\"ab\",null,\"b\"]");
    ASSERT_VALUE(bsTestGroups("((a)|b)*?c", "abc", 0), "[\"abc\",\"b\",null]");
    ASSERT_VALUE(bsTestGroups("((a)|b)*?c", "bac", 0), "[\"bac\",\"a\",\"a\"]");
    ASSERT_VALUE(bsTestGroups("(a*)*?b", "aab", 0), "[\"aab\",\"aa\"]");
    ASSERT_VALUE(bsTestGroups("(a|(b))*?x", "abx", 0), "[\"abx\",\"b\",\"b\"]");
    ASSERT_VALUE(bsTestGroups("(a?)*", "aa", 0), "[\"aa\",\"a\"]");
    ASSERT_VALUE(bsTestGroups("(a?){2,3}", "a", 0), "[\"a\",\"\"]");
    ASSERT_VALUE(bsTestGroups("(?:(a)|(b)|c){3}", "abc", 0), "[\"abc\",null,null]");
    ASSERT_VALUE(bsTestGroups("(?:(x)?y)*", "xyy", 0), "[\"xyy\",null]");
    ASSERT_VALUE(bsTestGroups("(\\1a)*", "aa", 0), "[\"aa\",\"a\"]");

    /* A named backreference may precede its group, and one to a name shared across an alternation's
       branches means the group that took part */
    ASSERT_VALUE(bsTestGroups("\\k<n>(?<n>a)", "a", 0), "[\"a\",\"a\"]");
    ASSERT_VALUE(bsTestGroups("(?:(?<n>a)|(?<n>b))\\k<n>", "bb", 0), "[\"bb\",null,\"b\"]");
    ASSERT_VALUE(bsTestGroups("(?:(?<n>a)|(?<n>b))\\k<n>", "aa", 0), "[\"aa\",\"a\",null]");
    ASSERT_VALUE_STRING(bsTestMatch("(?:(?<n>a)|(?<n>b))\\k<n>", "ab", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("(?:(?<n>a)|(?<n>b))\\k<n>", "xba", BS_REGEX_IGNORECASE), "null");
    ASSERT_VALUE_STRING(bsTestMatch("((?:(?!\\1c)){2}\xcf\x83{0}),", "\xcf\x82\xce\xa3", 0), "null");
    ASSERT_VALUE(bsTestGroups("(?:a)(b)", "ab", 0), "[\"ab\",\"b\"]");
    ASSERT_VALUE(bsTestGroups("(?<name>a)(b)", "ab", 0), "[\"ab\",\"a\",\"b\"]");
    ASSERT_VALUE(bsTestGroups("((a)(b))", "ab", 0), "[\"ab\",\"ab\",\"a\",\"b\"]");
    ASSERT_VALUE(bsTestGroups("(a)?b", "b", 0), "[\"b\",null]");

    /* Group names */
    BSValue regex = bsRegexNew("(?<year>[0-9]{4})-([0-9]{2})", 28, 0, NULL, 0);
    ASSERT_STR_EQ(bsStringData(bsRegexGroupNameValue(regex, 1)), "year");
    ASSERT_INT_EQ(bsRegexGroupNameValue(regex, 2).type, BS_NULL);
    ASSERT_INT_EQ(bsRegexGroupNameValue(regex, 9).type, BS_NULL);
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

    /* A lookbehind body matches right to left, as JavaScript's does: a greedy repeat takes from the right,
       a backreference can name a group to its right, and a lazy repeat gives one more to the left */
    ASSERT_VALUE(bsTestGroups("(?<=(a+))b", "aab", 0), "[\"b\",\"aa\"]");
    ASSERT_VALUE(bsTestGroups("(?<=(a*)(a*))b", "aab", 0), "[\"b\",\"\",\"aa\"]");
    ASSERT_VALUE(bsTestGroups("(?<=(\\w+) )x", "ab cd x", 0), "[\"x\",\"cd\"]");
    ASSERT_VALUE(bsTestGroups("(?<=(\\d+)(\\d+))x", "123x", 0), "[\"x\",\"1\",\"23\"]");
    ASSERT_VALUE(bsTestGroups("(?<=\\1(a))b", "aab", 0), "[\"b\",\"a\"]");
    ASSERT_VALUE_STRING(bsTestMatch("(?<=\\1(a))b", "ab", 0), "null");
    ASSERT_VALUE(bsTestGroups("(?<=(a)\\1)b", "aab", 0), "[\"b\",\"a\"]");
    ASSERT_VALUE(bsTestGroups("(?<=(a)\\1)b", "aAb", BS_REGEX_IGNORECASE), "[\"b\",\"A\"]");
    ASSERT_VALUE_STRING(bsTestMatch("(?<!(a+))b", "aab", 0), "null");
    ASSERT_VALUE(bsTestGroups("(?<=a(b+?))c", "abbc", 0), "[\"c\",\"bb\"]");
    ASSERT_VALUE(bsTestGroups("(?<=(a+)a)b", "aaab", 0), "[\"b\",\"aa\"]");
    ASSERT_VALUE(bsTestGroups("(?<=(a{1,2}))b", "aaab", 0), "[\"b\",\"aa\"]");
    ASSERT_VALUE(bsTestGroups("(?<=(a+?))b", "aab", 0), "[\"b\",\"a\"]");
    ASSERT_VALUE(bsTestGroups("(?<=(a*?)b)c", "aabc", 0), "[\"c\",\"\"]");
    ASSERT_VALUE(bsTestGroups("(?<=(ab|b))c", "abc", 0), "[\"c\",\"ab\"]");
    ASSERT_VALUE(bsTestGroups("(?<=(b|ab))c", "abc", 0), "[\"c\",\"b\"]");
    ASSERT_VALUE(bsTestGroups("(?<=(a){2})b", "aab", 0), "[\"b\",\"a\"]");
    ASSERT_VALUE(bsTestGroups("(?<=(?:a|b){2})c", "abc", 0), "[\"c\"]");
    ASSERT_VALUE(bsTestGroups("(?<=a(?=b))b", "ab", 0), "[\"b\"]");
    ASSERT_VALUE(bsTestGroups("(?<=(?<=(a))b)c", "abc", 0), "[\"c\",\"a\"]");
    ASSERT_VALUE(bsTestGroups("(?<=a.)b", "axb", 0), "[\"b\"]");
    ASSERT_VALUE(bsTestGroups("(?<=[a-z]{2})1", "ab1", 0), "[\"1\"]");
    ASSERT_VALUE(bsTestGroups("(?<=^a)b", "ab", 0), "[\"b\"]");
    ASSERT_VALUE(bsTestGroups("(?<=\\bab)c", "abc", 0), "[\"c\"]");
    ASSERT_VALUE(bsTestGroups("(?<=(a)?b)c", "bc", 0), "[\"c\",null]");
    ASSERT_VALUE(bsTestGroups("(?<=(a)?b)c", "abc", 0), "[\"c\",\"a\"]");
    ASSERT_VALUE(bsTestGroups("(?<=(a)|b)c", "bc", 0), "[\"c\",null]");
    ASSERT_VALUE(bsTestGroups("x(?<=x)y", "xy", 0), "[\"xy\"]");
    ASSERT_VALUE(bsTestGroups("(?<=\xc3\xa9+)b", "\xc3\xa9\xc3\xa9" "b", 0), "[\"b\"]");
    ASSERT_VALUE_STRING(bsTestMatch("(?<=a{2,}?)b", "ab", 0), "null");
    ASSERT_VALUE(bsTestGroups("(?<=(a+)aa)b", "aaab", 0), "[\"b\",\"a\"]");
    ASSERT_VALUE(bsTestGroups("(?<=a(a+))c", "aac", 0), "[\"c\",\"a\"]");
    ASSERT_VALUE_STRING(bsTestMatch("(?<=x(b+?))c", "abbc", 0), "null");
}


TEST(regex_ignorecase)
{
    ASSERT_VALUE_STRING(bsTestMatch("abc", "xABCy", BS_REGEX_IGNORECASE), "ABC");
    ASSERT_VALUE_STRING(bsTestMatch("ABC", "xabcy", BS_REGEX_IGNORECASE), "abc");
    ASSERT_VALUE_STRING(bsTestMatch("[a-c]+", "xABCy", BS_REGEX_IGNORECASE), "ABC");
    ASSERT_VALUE_STRING(bsTestMatch("[^a-c]+", "ABxy", BS_REGEX_IGNORECASE), "xy");
    ASSERT_VALUE_STRING(bsTestMatch("[0-9]+", "AB12", BS_REGEX_IGNORECASE), "12");

    /* Unicode simple case folding: a code point matches the ones sharing its upper case, but a fold across the
       ASCII boundary - the dotless i, the long s, the Kelvin sign - and an expanding upper case - the sharp s - do not */
    ASSERT_VALUE_STRING(bsTestMatch("\xc3\xa9", "x\xc3\x89", BS_REGEX_IGNORECASE), "\xc3\x89");
    ASSERT_VALUE_STRING(bsTestMatch("[\xc3\xa0-\xc3\xbf]", "\xc3\x80", BS_REGEX_IGNORECASE), "\xc3\x80");
    ASSERT_VALUE_STRING(bsTestMatch("[^\xc3\xa9]", "\xc3\x89", BS_REGEX_IGNORECASE), "null");
    ASSERT_VALUE_STRING(bsTestMatch("\xcf\x83", "\xcf\x82", BS_REGEX_IGNORECASE), "\xcf\x82");
    ASSERT_VALUE_STRING(bsTestMatch("[\xcf\x82]", "\xce\xa3", BS_REGEX_IGNORECASE), "\xce\xa3");
    ASSERT_VALUE_STRING(bsTestMatch("\xc2\xb5", "x\xce\x9c", BS_REGEX_IGNORECASE), "\xce\x9c");
    ASSERT_VALUE_STRING(bsTestMatch("[\xc2\xb5]", "\xce\xbc", BS_REGEX_IGNORECASE), "\xce\xbc");
    ASSERT_VALUE_STRING(bsTestMatch("\xcd\x85", "\xce\xb9", BS_REGEX_IGNORECASE), "\xce\xb9");
    ASSERT_VALUE_STRING(bsTestMatch("(\xc3\xa9)\\1", "\xc3\xa9\xc3\x89", BS_REGEX_IGNORECASE), "\xc3\xa9\xc3\x89");
    ASSERT_VALUE_STRING(bsTestMatch("\xc3\x9f", "\xe1\xba\x9e", BS_REGEX_IGNORECASE), "null");
    ASSERT_VALUE_STRING(bsTestMatch("\xc4\xb1", "I", BS_REGEX_IGNORECASE), "null");
    ASSERT_VALUE_STRING(bsTestMatch("i", "\xc4\xb0", BS_REGEX_IGNORECASE), "null");
    ASSERT_VALUE_STRING(bsTestMatch("k", "\xe2\x84\xaa", BS_REGEX_IGNORECASE), "null");
    ASSERT_VALUE_STRING(bsTestMatch("[k]", "\xe2\x84\xaa", BS_REGEX_IGNORECASE), "null");
    ASSERT_VALUE_STRING(bsTestMatch("[\xe2\x84\xaa]", "k", BS_REGEX_IGNORECASE), "null");
    ASSERT_VALUE_STRING(bsTestMatch("s", "\xc5\xbf", BS_REGEX_IGNORECASE), "null");
    ASSERT_VALUE_STRING(bsTestMatch("[a-z]", "\xc5\xbf", BS_REGEX_IGNORECASE), "null");
    ASSERT_VALUE_STRING(bsTestMatch("\xc3\xa9+", "\xc3\x89\xc3\xa9x", BS_REGEX_IGNORECASE), "\xc3\x89\xc3\xa9");
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

    /* An anchored pattern runs at the start position alone, where "^" fails past the first; an
       alternative's "^" is reached by the scanning run at each position a match can begin */
    ASSERT_VALUE(bsTestExecute("return regexMatchAll(regexNew('^a'), 'aaa')"),
                 "[{\"groups\":{\"0\":\"a\"},\"index\":0,\"input\":\"aaa\"}]");
    ASSERT_VALUE(bsTestExecute("return regexMatch(regexNew('b|^a'), 'xa')"), "null");
    ASSERT_VALUE(bsTestExecute("return regexMatch(regexNew('b|^a'), 'xab')"),
                 "{\"groups\":{\"0\":\"b\"},\"index\":2,\"input\":\"xab\"}");
}


TEST(regex_subject_long)
{
    /* ASCII subjects match the original bytes and do not allocate a code-point buffer */
    BSValue string = bsTestRepeat(NULL, "x", 200, "target");
    BSRegexSubject subject;
    bsRegexSubjectInit(&subject, string);
    ASSERT_NULL(subject.codes);
    ASSERT_NULL(subject.owned);
    ASSERT_NOT_NULL(subject.bytes);
    ASSERT_INT_EQ(subject.length, 206);
    BSValue regex = bsRegexNew("target", 6, 0, NULL, 0);
    BSRegexMatch match;
    ASSERT_TRUE(bsRegexSearch(regex, &subject, 0, &match));
    ASSERT_INT_EQ(match.begin, 200);
    bsRegexSubjectFree(&subject);
    bsRelease(string);
    bsRelease(regex);

    /* A long non-ASCII subject still widens into a heap code-point buffer */
    BSValue unicodeString = bsTestRepeat(NULL, "\xc3\xa9", 70, NULL);
    BSRegexSubject unicodeSubject;
    bsRegexSubjectInit(&unicodeSubject, unicodeString);
    ASSERT_NOT_NULL(unicodeSubject.codes);
    ASSERT_NOT_NULL(unicodeSubject.owned);
    ASSERT_NULL(unicodeSubject.bytes);
    ASSERT_INT_EQ(unicodeSubject.length, 70);
    BSValue unicodeRegex = bsRegexNew("\xc3\xa9", 2, 0, NULL, 0);
    ASSERT_TRUE(bsRegexSearch(unicodeRegex, &unicodeSubject, 0, &match));
    ASSERT_INT_EQ(match.begin, 0);
    bsRegexSubjectFree(&unicodeSubject);
    bsRelease(unicodeString);
    bsRelease(unicodeRegex);
}


TEST(regex_compile_errors)
{
    bsTestRegexError("(", "missing ), unterminated subpattern at position 0");
    bsTestRegexError("(?<a>x", "missing ), unterminated subpattern at position 0");
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
    bsTestRegexError("[\\d-z]", "bad character range \\d-z at position 1");
    bsTestRegexError("0[\\x41-0", "bad character range \\x-0 at position 4");
    bsTestRegexError("[\\x41-\\x30]", "bad character range \\x-\\x at position 5");
    bsTestRegexError("\\477", "octal escape value \\477 outside of range 0-0o377 at position 0");

    /* Positions count code points, as Python's do */
    bsTestRegexError("\xc3\xa9(?!", "missing ), unterminated subpattern at position 1");
    bsTestRegexError("a\xc3\xa9[b-a]", "bad character range b-a at position 3");
    bsTestRegexError("\xc3\xa9(?<n>a)\\k<m>", "unknown group name 'm' at position 13");
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
    bsTestRegexError("[a-\\b]", "bad character range a-\\b at position 1");
    bsTestRegexError("(a)\\99999999999", "invalid group reference 2147483647 at position 4");
    bsTestRegexError("a*?*", "multiple repeat at position 3");
    bsTestRegexError("\\999", "invalid group reference 999 at position 1");

    /* A backreference to a group the pattern never defines - a forward reference to one it does is fine */
    bsTestRegexError("(a)\\2", "invalid group reference 2 at position 4");
    bsTestRegexError("(a)(b)\\12", "invalid group reference 12 at position 7");
    bsTestRegexError("(a)\\2)", "invalid group reference 2 at position 4");
    bsTestRegexError("\\2\\1(a)", "invalid group reference 2 at position 1");

    /* Too many capture groups */
    BSValue pattern = bsTestRepeat(NULL, "(a)", 130, NULL);
    char error[BS_REGEX_ERROR_MAX];
    bsRelease(bsRegexNew(bsStringData(pattern), bsStringSize(pattern), 0, error, sizeof(error)));
    ASSERT_STR_EQ(error, "sorry, but this version only supports 127 groups at position 381");
    bsRelease(pattern);

    /* The error argument is optional */
    bsRelease(bsRegexNew("(", 1, 0, NULL, 0));
    bsRelease(bsRegexNew("a", 1, 0, NULL, 0));

    /* A group name is shared only across the branches of one alternation, as in JavaScript; a name is a
       BareScript identifier; an assertion cannot be repeated */
    bsTestRegexError("(?<n>a)(?<n>b)", "redefinition of group name 'n' as group 2; was group 1 at position 12");
    bsTestRegexError("((?<n>a)|c)(?<n>b)", "redefinition of group name 'n' as group 3; was group 2 at position 16");
    bsTestRegexError("(?:(?<n>a))(?:(?<n>b))", "redefinition of group name 'n' as group 2; was group 1 at position 19");
    /* A name may be shared across top-level alternatives */
    BSValue shared = bsRegexNew("(?:(?<n>a)|x)|(?<n>b)", 21, 0, error, sizeof(error));
    ASSERT_INT_EQ(shared.type, BS_REGEX);
    ASSERT_STR_EQ(error, "");
    bsRelease(shared);
    bsTestRegexError("(?<1a>x)", "bad character in group name '1a' at position 4");
    bsTestRegexError("(?=a)*", "nothing to repeat at position 5");
    bsTestRegexError("(?<!a)?", "nothing to repeat at position 6");
    bsTestRegexError("\\k<n>", "unknown group name 'n' at position 4");
}


TEST(regex_depth_limit)
{
    /* A pathological pattern gives up on its step budget rather than hanging */
    BSValue subject = bsTestRepeat(NULL, "a", 30, NULL);
    ASSERT_VALUE_STRING(bsTestMatch("(a|aa)+$b", bsStringData(subject), 0), "null");
    bsRelease(subject);

    /* A chain of alternations with no repeats, and nested repeats with no alternations */
    BSValue chain = bsTestRepeat(NULL, "(?:a|a)", 24, "b");
    subject = bsTestRepeat(NULL, "a", 24, NULL);
    ASSERT_VALUE_STRING(bsTestMatch(bsStringData(chain), bsStringData(subject), 0), "null");
    bsRelease(subject);
    bsRelease(chain);

    subject = bsTestRepeat(NULL, "ab", 30, NULL);
    ASSERT_VALUE_STRING(bsTestMatch("(?:(?:ab)*)*c", bsStringData(subject), 0), "null");
    bsRelease(subject);

    /* A long simple repeat matches iteratively, without recursion */
    BSValue longSubject = bsTestRepeat(NULL, "x", 20000, "y");
    BSValue longRegex = bsRegexNew("x*y", 3, 0, NULL, 0);
    BSRegexSubject subjectCodes;
    bsRegexSubjectInit(&subjectCodes, longSubject);
    BSRegexMatch match;
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

    /* NUL is not a metacharacter, though strchr finds it in the metacharacter string */
    BSValue nul = bsRegexEscape(bsStringNewSize("a\0b", 3));
    ASSERT_INT_EQ(bsStringSize(nul), 3);
    ASSERT_INT_EQ(bsStringData(nul)[1], 0);
    bsRelease(nul);
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

    /* An invalid escape as a character class range's high bound */
    bsTestRegexError("[a-\\x4G]", "incomplete escape \\x4 at position 3");

    /* An invalid sub-pattern inside a group */
    bsTestRegexError("(\\x4G)", "incomplete escape \\x4 at position 1");
    bsTestRegexError("(?:[c-a])", "bad character range c-a at position 4");

    /* A lookbehind whose length bound saturates on an unbounded body */
    ASSERT_VALUE_STRING(bsTestMatch("(x)?(?<=\\1{2})b", "ab", 0), "b");
    ASSERT_VALUE_STRING(bsTestMatch("(x)?(?<=\\1{2,4})b", "ab", 0), "b");

    /* Lazy simple repeats that cannot meet their minimum */
    ASSERT_VALUE_STRING(bsTestMatch("a{3,}?b", "aab", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("x{2,}?", "", 0), "null");

    /* A lazy non-simple repeat with a zero minimum tries its continuation first */
    ASSERT_VALUE_STRING(bsTestMatch("x(?:ab)*?y", "xaby", 0), "xaby");
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab)??c", "abc", 0), "abc");

    /* A greedy simple repeat that matches zero times and whose continuation fails */
    ASSERT_VALUE_STRING(bsTestMatch("a*b", "c", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("a*$", "b", 0), "");

    /* A lazy simple repeat whose minimum cannot be met */
    ASSERT_VALUE_STRING(bsTestMatch("a{3,}?", "aa", 0), "null");

    /* A non-simple repeat with a zero maximum matches nothing */
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab){0}c", "c", 0), "c");
    ASSERT_VALUE_STRING(bsTestMatch("(a){0}b", "b", 0), "b");
}


TEST(regex_program)
{
    /* Failing instructions: a folded literal, dot-all at the end, and both general lookarounds */
    ASSERT_VALUE_STRING(bsTestMatch("abc", "ABD", BS_REGEX_IGNORECASE), "null");
    ASSERT_VALUE_STRING(bsTestMatch("a.", "a", BS_REGEX_DOTALL), "null");
    ASSERT_VALUE_STRING(bsTestMatch("(?=ab)a", "ac", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("(?<=ab)c", "xbc", 0), "null");

    /* A lookbehind body that matches but ends past the anchor */
    ASSERT_VALUE_STRING(bsTestMatch("(?<=a+)c", "aabc", 0), "null");

    /* A single-code-point lookaround at the end of a non-ASCII subject, and a class body */
    ASSERT_VALUE_STRING(bsTestMatch("\xc3\xa9(?=a)", "\xc3\xa9", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("(?<=[a-c])x", "bx", 0), "x");
    ASSERT_VALUE_STRING(bsTestMatch("[ab]+?c", "abc", 0), "abc");
    ASSERT_VALUE_STRING(bsTestMatch("a+?b", "AAB", BS_REGEX_IGNORECASE), "AAB");
    ASSERT_VALUE_STRING(bsTestMatch(".+?b", "x\nb", BS_REGEX_DOTALL), "x\nb");

    /* A greedy simple repeat giving back past positions that do not hold the following literal */
    ASSERT_VALUE_STRING(bsTestMatch("a*bc", "aabab", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("a*bc", "aababc", 0), "abc");
    ASSERT_VALUE_STRING(bsTestMatch("[ab]*bc", "abab", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("[ab]*bc", "ababbc", 0), "ababbc");

    /* An alternation too wide to index backtracks through its alternatives one at a time */
    BSValue wide = bsTestRepeat("(?:a", "|b|a", 33, ")c");
    ASSERT_VALUE_STRING(bsTestMatch(bsStringData(wide), "ax", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch(bsStringData(wide), "bx", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch(bsStringData(wide), "bc", 0), "bc");
    bsRelease(wide);
    wide = bsTestRepeat("(?:a", "|b|a", 33, "|x*)c");
    ASSERT_VALUE_STRING(bsTestMatch(bsStringData(wide), "zc", 0), "c");
    bsRelease(wide);
}


TEST(regex_alternation_index)
{
    /* A wide alternation tries only the alternatives that can begin with the code point at hand */
    static const char *wide = "(?:aa|bb|cc|dd|ee|ff|gg|hh|ii)";
    ASSERT_VALUE_STRING(bsTestMatch(wide, "xxhhxx", 0), "hh");
    ASSERT_VALUE_STRING(bsTestMatch(wide, "xxhixx", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch(wide, "", 0), "null");

    /* An alternative that can match the empty string or begin with anything is always tried, and
     * one beginning with a code point past the table matches by its "high" set */
    static const char *mixed = "(?:aa|bb|cc|dd|ee|ff|gg|\xe6\xbc\xa2x|z*)";
    ASSERT_VALUE_STRING(bsTestMatch(mixed, "qq", 0), "");
    ASSERT_VALUE_STRING(bsTestMatch(mixed, "zzz", 0), "zzz");
    ASSERT_VALUE_STRING(bsTestMatch("(?:aa|bb|cc|dd|ee|ff|gg|\xe6\xbc\xa2x|q)", "..\xe6\xbc\xa2x", 0),
                        "\xe6\xbc\xa2x");
    ASSERT_VALUE_STRING(bsTestMatch("(?:aa|bb|cc|dd|ee|ff|gg|\xc3\xa9x|q)", "..\xc3\xa9x", 0), "\xc3\xa9x");
    ASSERT_VALUE_STRING(bsTestMatch("(?:aa|bb|cc|dd|ee|ff|gg|\xe6\xbc\xa2x|q)", "..\xe6\xbc\xa3x", 0), "null");
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


TEST(regex_follow_set)
{
    /* A general repeat skips a continuation that cannot begin at the position: a lazy repeat's first
       try, a greedy repeat's way back. The matches are what JavaScript's are; the follow set only prunes. */
    ASSERT_VALUE_STRING(bsTestMatch("^\\s*(?<cell>(?:\\\\\\||[^|])*?)\\s*\\|", "  ab\\| cd |x", 0), "  ab\\| cd |");
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab|c)*?x", "abcab", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab|c)*x", "abcabx", 0), "abcabx");
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab|c)*?d", "abcabd", 0), "abcabd");
    /* A continuation that can be empty, and one beginning with anything, are not follow sets */
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab|c)*?x?$", "abcab", 0), "abcab");
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab|c)*?.x", "abcabx", 0), "bx");
    /* The follow set through a group, into a repeat body, and past the pattern's end */
    ASSERT_VALUE_STRING(bsTestMatch("((?:ab|c)*?)d", "abcabd", 0), "abcabd");
    ASSERT_VALUE_STRING(bsTestMatch("(?:a(?:b|c)*?d)*?e", "abcdacde", 0), "abcdacde");
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab|c)*?", "abc", 0), "");
    /* Inside a lookahead and a lookbehind the continuation is unknown */
    ASSERT_VALUE_STRING(bsTestMatch("(?=(?:ab|c)*?d)", "cabd", 0), "");
    ASSERT_VALUE_STRING(bsTestMatch("(?<=(?:ab|c)*?x)y", "abxy", 0), "y");
    /* Case folding and non-ASCII code points in the follow set */
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab|c)*?D", "abcd", BS_REGEX_IGNORECASE), "abcd");
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab|c)*?(?:\xc3\xa9|f)", "abc\xc3\xa9", 0), "abc\xc3\xa9");
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab|c)*?(?:\xc3\xa9|f)", "abc", 0), "null");
}


TEST(regex_sequences_repeat)
{
    /* A repeat of an alternation of atom sequences matches iteratively, in the recursive order */
    ASSERT_VALUE_STRING(bsTestMatch("(?:a|ab)*c", "ababc", 0), "ababc");
    ASSERT_VALUE_STRING(bsTestMatch("(?:aa|a){3}b", "aaaab", 0), "aaaab");
    ASSERT_VALUE_STRING(bsTestMatch("x(?:ab|a){2}y", "xabay", 0), "xabay");
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab){2}", "ababab", 0), "abab");
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab){2,3}c", "ababababc", 0), "abababc");
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab){2,3}c", "abc", 0), "null");
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab)*", "xab", 0), "");

    /* A lazy repeat tries the continuation before each iteration */
    ASSERT_VALUE_STRING(bsTestMatch("(?:a|b)*?b", "aab", 0), "aab");
    ASSERT_VALUE_STRING(bsTestMatch("^(?:ab|cd)*?$", "abcd", 0), "abcd");
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab){2,}?", "ababab", 0), "abab");
    ASSERT_VALUE_STRING(bsTestMatch("^(?:ab|a)+?c", "aabc", 0), "aabc");
    ASSERT_VALUE_STRING(bsTestMatch("(?:ab){2,}?c", "abc", 0), "null");

    /* A non-ASCII subject matches by code point */
    ASSERT_VALUE_STRING(bsTestMatch("(?:\xc3\xa9|ab)+", "ab\xc3\xa9\xc3\xa9x", 0), "ab\xc3\xa9\xc3\xa9");

    /* Bodies the iterative matcher leaves to recursion: an empty alternative, a capture, a non-atom, too many branches */
    ASSERT_VALUE_STRING(bsTestMatch("(?:|a)*b", "aab", 0), "aab");
    ASSERT_VALUE_STRING(bsTestMatch("(a|b)*c", "abc", 0), "abc");
    ASSERT_VALUE_STRING(bsTestMatch("(a|b)*?c", "abc", 0), "abc");
    ASSERT_VALUE_STRING(bsTestMatch("(a){1,}?b", "aab", 0), "aab");
    ASSERT_VALUE_STRING(bsTestMatch("(?:a|(b))*c", "abc", 0), "abc");
    BSValue pattern = bsTestRepeat("(?:a", "|a", 255, ")*b");
    ASSERT_VALUE_STRING(bsTestMatch(bsStringData(pattern), "aab", 0), "aab");
    bsRelease(pattern);
}


TEST(regex_sequences_repeat_long_literal)
{
    /* The parser's string literal body is such a repeat - a long literal costs no C stack */
    BSValue script = bsTestRepeat("return stringLength('", "a", 20000, "\\'\xc3\xa9')");
    ASSERT_VALUE(bsTestExecute(bsStringData(script)), "20002");
    bsRelease(script);

    script = bsTestRepeat("return stringLength(\"", "a\\\\", 20000, "\")");
    ASSERT_VALUE(bsTestExecute(bsStringData(script)), "40000");
    bsRelease(script);
}
