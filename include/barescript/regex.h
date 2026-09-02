/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * A targeted regular expression implementation for BareScript
 *
 * The engine is a backtracking matcher over a compiled node tree. It implements the subset of
 * JavaScript regular expression syntax that BareScript's regex library functions expose:
 *
 *   literals  . [...] [^...] ( ) (?: ) (?<name> ) (?= ) (?! ) (?<= ) (?<! ) |
 *   escapes   \d \D \w \W \s \S \b \B \n \r \t \f \v \0 \xHH \uHHHH \k<name> \1 - \9
 *   repeats   * + ? {n} {n,} {n,m} and their lazy "?" forms
 *   anchors   ^ $
 *   flags     i (case-insensitive), m (multi-line), s (dot matches newline)
 *
 * Matching is over Unicode code points, so match indexes agree with the string library's indexes.
 */

#ifndef BARESCRIPT_REGEX_H
#define BARESCRIPT_REGEX_H

#include "value.h"
#include "export.h"

#ifdef __cplusplus
extern "C" {
#endif

BS_VISIBILITY_BEGIN


/* The regular expression flags */
#define BS_REGEX_IGNORECASE 0x01
#define BS_REGEX_MULTILINE  0x02
#define BS_REGEX_DOTALL     0x04

/* The maximum number of capture groups in a pattern */
#define BS_REGEX_GROUPS_MAX 128

/* The size of a compilation error message buffer */
#define BS_REGEX_ERROR_MAX 128


/* A regular expression match's capture group span, in code point indexes */
typedef struct BSRegexSpan {
    size_t begin;
    size_t end;
} BSRegexSpan;


/* A regular expression match */
typedef struct BSRegexMatch {
    size_t begin;
    size_t end;
    size_t groupCount; /* the number of capture groups, including group zero */
    BSRegexSpan groups[BS_REGEX_GROUPS_MAX];
    bool matched[BS_REGEX_GROUPS_MAX];
} BSRegexMatch;


/*
 * Compile a regular expression. Returns an owned regex value, or a null value if the pattern is
 * invalid.
 *
 * If "error" is non-NULL it receives the compilation error message, which is empty on success.
 * The messages, and the pattern positions they report, are the ones Python's "re" module produces
 * - the module the Python implementation's regexNew compiles with - so a BareScript program sees
 * the same diagnostic on both implementations. "errorSize" should be BS_REGEX_ERROR_MAX.
 */
BSValue bsRegexNew(const char *pattern, size_t patternSize, unsigned flags, char *error,
                   size_t errorSize);

/* Get a compiled regular expression's pattern source and flags */
const char *bsRegexPattern(BSValue regex);
unsigned bsRegexFlags(BSValue regex);

/* Get a compiled regular expression's capture group count, including group zero */
size_t bsRegexGroupCount(BSValue regex);

/* Get a capture group's name, or NULL if the group is not named */
const char *bsRegexGroupName(BSValue regex, size_t group);

/*
 * A pre-decoded subject string. Decoding a string once and matching against it repeatedly avoids
 * re-decoding UTF-8 for every match of a global search.
 */
typedef struct BSRegexSubject {
    const uint32_t *codes;       /* widened code points, or NULL for an ASCII subject */
    const unsigned char *bytes;  /* original bytes; for ASCII, code point i is bytes[i] */
    size_t length;
    uint32_t *owned; /* the heap buffer to free, or NULL if codes is NULL or inline */
    uint32_t inline_[64];
} BSRegexSubject;

void bsRegexSubjectInit(BSRegexSubject *subject, BSValue string);
void bsRegexSubjectFree(BSRegexSubject *subject);

/*
 * Search for the first match at or after "start". Returns true if a match was found and fills in
 * "match".
 */
bool bsRegexSearch(BSValue regex, const BSRegexSubject *subject, size_t start, BSRegexMatch *match);

/* Escape a string's regular expression metacharacters - returns an owned string value */
BSValue bsRegexEscape(BSValue string);


BS_VISIBILITY_END

#ifdef __cplusplus
}
#endif

#endif
