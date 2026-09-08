/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * A targeted regular expression implementation for BareScript
 *
 * A pattern parses to a tree of nodes chained by "next" pointers, which compiles to a linear
 * program and is then freed. Matching runs the program in one loop with an explicit backtrack
 * stack, so no pattern recurses on the C stack beyond one call per lookaround body. Quantifiers
 * whose body matches exactly one code point - the common case, "\\s*", "[0-9]+", ".*" - scan in
 * one loop and give back through one backtrack entry.
 */

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "barescript/regex.h"

#include "internal.h"


/* Threaded dispatch for the matcher's program loop, where the compiler supports label addresses */
#if defined(__GNUC__) || defined(__clang__)
#define RX_THREADED_DISPATCH 1
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-label-as-value"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

/*
 * The maximum backtracking steps per match start position
 *
 * A backtracking engine can explore exponentially many paths for patterns like "(a|aa)+$" - the
 * classic catastrophic backtracking case. The budget bounds that exploration so a script cannot
 * hang the runtime with a pathological pattern; it is far above what any non-pathological pattern
 * needs, so ordinary matching never reaches it.
 */
#define RX_STEPS_MAX 1000000


/* The predefined character class flags */
#define RX_CLASS_DIGIT     0x01
#define RX_CLASS_NOTDIGIT  0x02
#define RX_CLASS_WORD      0x04
#define RX_CLASS_NOTWORD   0x08
#define RX_CLASS_SPACE     0x10
#define RX_CLASS_NOTSPACE  0x20


typedef enum {
    RX_CHAR,
    RX_ANY,
    RX_CLASS,
    RX_ALT,
    RX_GROUP,
    RX_REPEAT,
    RX_BOL,
    RX_EOL,
    RX_WORD_BOUNDARY,
    RX_NOT_WORD_BOUNDARY,
    RX_BACKREF,
    RX_LOOKAHEAD,
    RX_LOOKBEHIND
} RxKind;


/* A character class - a node's, and then the program's once the node tree is gone */
typedef struct RxClass {
    uint32_t *ranges; /* pairs of inclusive code point bounds */
    size_t rangeCount;
    unsigned classes;
    bool negate;
    bool fold;        /* matched case-insensitively */
    uint8_t ascii[16]; /* membership of the code points 0 - 127, with flags and negation applied */
} RxClass;


typedef struct RxNode RxNode;

struct RxNode {
    RxKind kind;
    RxNode *next;
    union {
        uint32_t ch;
        RxClass cls;
        struct {
            RxNode **branches;
            size_t count;
        } alt;
        struct {
            RxNode *sub;
            size_t group;
        } group;
        struct {
            size_t group;     /* RX_BACKREF: the group, or the first of several sharing a referenced name */
            uint32_t *groups; /* the groups sharing the name when there are several; the one that took part matches */
            size_t count;
            BSValue name;     /* a named reference's name, until its groups are resolved after the parse */
        } backref;
        struct {
            RxNode *sub;
            int min;
            int max; /* -1 for unbounded */
            bool greedy;
            uint32_t groupFirst; /* the body's capture groups, [groupFirst, groupEnd) */
            uint32_t groupEnd;
        } repeat;
        struct {
            RxNode *sub;
            bool negate;
        } look;
    } u;
};


/* Nodes are allocated in chunks so a typical pattern is one malloc instead of one per node */
#define RX_CHUNK_NODES 32

typedef struct RxNodeChunk {
    struct RxNodeChunk *next;
    RxNode nodes[RX_CHUNK_NODES];
    size_t used;
} RxNodeChunk;


/*
 * The set of code points a match can begin with
 *
 * A search skips any position whose code point cannot begin a match, which turns the scan for a
 * pattern like the markdown span alternation - thirteen alternatives, each starting with one of a
 * handful of punctuation characters - from a full match attempt per position into a table lookup.
 * The set is only used when it is exact: a pattern that can match the empty string, or that can
 * begin with anything, disables it.
 */
typedef struct RxFirstSet {
    uint64_t bits[4]; /* membership of the code points 0 - 255 */
    bool high;        /* a match can begin with a code point of 256 or more */
    bool any;         /* a match can begin with anything, or be empty, so the set is not usable */
} RxFirstSet;


static inline bool rxFirstHas(const RxFirstSet *set, uint32_t code)
{
    return code >= 256 ? set->high : (set->bits[code >> 6] >> (code & 63)) & 1u;
}


/*
 * A wide alternation's alternatives indexed by first code point: "codes" holds, per ASCII code
 * point, the set of alternatives that can begin with it as a bit per alternative, so the
 * alternation tries just those, in order, instead of testing every alternative's set; "high"
 * holds the alternatives that can begin with any code point past ASCII, and "always" the
 * alternatives whose sets are not usable. Built for alternations of eight to sixty-four
 * alternatives - the markdown span alternation has sixteen, a highlight keyword list up to
 * sixty-two.
 */
#define RX_ALT_INDEX_MIN 8
#define RX_ALT_INDEX_MAX 64

typedef struct RxAltIndex {
    uint64_t codes[128];
    uint64_t high;   /* the alternatives that can begin with a code point of 128 or more */
    uint64_t always;
} RxAltIndex;


static inline unsigned rxLowestBit64(uint64_t mask)
{
#if defined(__GNUC__)
    return (unsigned) __builtin_ctzll(mask);
#else
    unsigned bit = 0;
    while ((mask & 1u) == 0) {
        mask >>= 1;
        bit++;
    }
    return bit;
#endif
}


typedef struct RxInst RxInst;
struct BSRegex;
typedef struct RxCompiler RxCompiler;
static void rxEmitProgram(RxCompiler *compiler);
static void rxProgramFree(struct BSRegex *regex);

struct BSRegex {
    int32_t refcount;
    bool anchored; /* every alternative begins with "^", so only the start position can match */
    RxFirstSet first;
    int firstByte;           /* the one byte a match can begin with, or -1 - the ASCII search scans with memchr */
    uint8_t firstBytes[256]; /* the first set's membership by byte, for the ASCII search scan */
    size_t groupCount;
    BSValue *groupNames;  /* groupCount entries, or NULL if no group is named */
    bool uniqueNames;     /* no two groups share a name, so a match model can append each */
    RxInst *prog;         /* the compiled program, and the tables its instructions refer to */
    struct RxClass *classes;
    size_t classCount;
    struct RxAlt *alts;
    size_t altCount;
    struct RxRepeatGroups *repeatGroups; /* per counted repeat, the body's capture groups */
    uint32_t *backrefGroups; /* the groups a backreference to a shared name may mean, by table index */
    uint32_t repeatCount; /* the counted repeats, each with a counter slot */
};


/*
 * Compile
 */


/* A node that matches exactly one code point and captures nothing */
static bool rxIsSimple(const RxNode *node)
{
    return node->next == NULL && (node->kind == RX_CHAR || node->kind == RX_ANY || node->kind == RX_CLASS);
}


/* The single-code-point node a lookaround's sub-pattern - always an alternation - reduces to, or NULL */
static RxNode *rxSimpleAtom(RxNode *alt)
{
    RxNode *node = alt->u.alt.count == 1 ? alt->u.alt.branches[0] : NULL;
    return (node != NULL && rxIsSimple(node)) ? node : NULL;
}


typedef struct RxCompiler {
    const char *pattern;
    size_t size;
    size_t offset;
    unsigned flags;
    BSRegex *regex;
    RxNode *root;
    RxNodeChunk *chunks; /* the parse tree; freed once the program is compiled */
    char *error;      /* the caller's message buffer, empty until a failure */
    size_t errorSize;
    bool failed;
    BSValue groupNames[BS_REGEX_GROUPS_MAX];
    struct RxBackref {
        int group;
        size_t offset;
    } *backrefs; /* the numbered backreferences, checked against the group count once the pattern is parsed */
    size_t backrefCount;
    size_t backrefCap;
    struct RxNamedRef {
        RxNode *node;
        size_t offset;
    } *namedRefs; /* the named backreferences, resolved to their groups once the pattern is parsed */
    size_t namedRefCount;
    size_t namedRefCap;
    /*
     * The alternations open around the parse point - each an id and its current branch - so a
     * group name reused where both groups could take part in one match is rejected: JavaScript
     * shares a name only across the branches of one alternation
     */
    struct RxPathEntry {
        uint32_t alternation;
        uint32_t branch;
    } *path;
    size_t pathCount;
    size_t pathCap;
    uint32_t alternationCount;
    struct {
        struct RxPathEntry *entries;
        size_t count;
    } groupPaths[BS_REGEX_GROUPS_MAX]; /* a named group's path */
} RxCompiler;


/* The word characters - a name's characters, and "\\w" */
static bool rxIsWordCode(uint32_t ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
}


/* True if a "(?<name>" or "\k<name>" translation begins at an offset */
static bool rxIsTranslated(const char *pattern, size_t size, size_t ix)
{
    if (ix + 3 > size) {
        return false;
    }
    bool named = (pattern[ix] == '(' && pattern[ix + 1] == '?' && pattern[ix + 2] == '<');
    bool backref = (pattern[ix] == '\\' && pattern[ix + 1] == 'k' && pattern[ix + 2] == '<');
    if (!named && !backref) {
        return false;
    }
    size_t nameIx = ix + 3;
    size_t end = nameIx;
    while (end < size && rxIsWordCode((unsigned char) pattern[end])) {
        end++;
    }
    return end != nameIx && end < size && pattern[end] == '>';
}


/*
 * The position a compilation error reports - in code points, as Python counts them
 *
 * The Python implementation compiles a translated pattern - "(?<name>" becomes "(?P<name>" and
 * "\k<name>" becomes "(?P=name)", each exactly one character longer - so the position it reports
 * is this pattern's position plus the number of translations that precede it.
 */
static size_t rxErrorPosition(const RxCompiler *compiler, size_t position)
{
    size_t bytes = position < compiler->size ? position : compiler->size;
    size_t translated = bsUTF8Length(compiler->pattern, bytes) + (position - bytes);
    for (size_t ix = 0; ix < position && ix < compiler->size; ix++) {
        if (rxIsTranslated(compiler->pattern, compiler->size, ix)) {
            translated++;
        }
    }
    return translated;
}


/*
 * The size of the pattern token at "offset"
 *
 * An escape is two characters, everything else is one. Python names the whole escape in its
 * "unknown extension" messages, so an escape after "(?" reports as "?\\d", not "?\\".
 */
static size_t rxTokenSize(const RxCompiler *compiler, size_t offset)
{
    if (compiler->pattern[offset] == '\\' && offset + 1 < compiler->size) {
        return 2;
    }
    return 1;
}


/* Report a compilation failure, in the form Python's "re" module reports it */
static void rxError(RxCompiler *compiler, size_t position, const char *format, ...)
{
    compiler->failed = true;
    if (compiler->error == NULL) {
        return;
    }
    char message[BS_REGEX_ERROR_MAX];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    snprintf(compiler->error, compiler->errorSize, "%s at position %zu", message,
             rxErrorPosition(compiler, position));
}


/* The canonical form of a code point for case-insensitive matching - the ASCII letters in line */
static inline uint32_t rxCanon(uint32_t ch)
{
    if (ch < 128) {
        return (ch >= 'a' && ch <= 'z') ? ch - 32 : ch;
    }
    return bsUnicodeCanon(ch);
}


/* Whether a code point is one of JavaScript's line terminators */
static bool rxIsLineTerminator(uint32_t ch)
{
    return ch == '\n' || ch == '\r' || ch == 0x2028 || ch == 0x2029;
}


static RxNode *rxNodeNew(RxCompiler *compiler, RxKind kind)
{
    RxNodeChunk *chunk = compiler->chunks;
    if (chunk == NULL || chunk->used == RX_CHUNK_NODES) {
        chunk = bsAlloc(sizeof(RxNodeChunk));
        chunk->next = compiler->chunks;
        chunk->used = 0;
        compiler->chunks = chunk;
    }
    RxNode *node = &chunk->nodes[chunk->used++];
    memset(node, 0, sizeof(RxNode));
    node->kind = kind;
    return node;
}


static void rxClassRange(RxNode *node, uint32_t lo, uint32_t hi)
{
    size_t count = node->u.cls.rangeCount;
    node->u.cls.ranges = bsRealloc(node->u.cls.ranges, (count + 1) * 2 * sizeof(uint32_t));
    node->u.cls.ranges[count * 2] = lo;
    node->u.cls.ranges[count * 2 + 1] = hi;
    node->u.cls.rangeCount = count + 1;
}


static RxNode *rxCharNode(RxCompiler *compiler, uint32_t ch)
{
    RxNode *node = rxNodeNew(compiler, RX_CHAR);
    node->u.ch = (compiler->flags & BS_REGEX_IGNORECASE) != 0 ? rxCanon(ch) : ch;
    return node;
}


/*
 * The rest of a legacy octal escape whose first digit is "first": up to two more octal digits, one
 * when the first is 4 to 7, as both references read it.
 */
static uint32_t rxOctal(RxCompiler *compiler, char first)
{
    uint32_t value = (uint32_t) (first - '0');
    size_t limit = first <= '3' ? 2 : 1;
    for (size_t count = 0; count < limit && compiler->offset < compiler->size; count++) {
        char digit = compiler->pattern[compiler->offset];
        if (digit < '0' || digit > '7') {
            break;
        }
        value = value * 8 + (uint32_t) (digit - '0');
        compiler->offset++;
    }
    return value;
}


/*
 * Parse a fixed-length hexadecimal escape
 *
 * On failure the escape is reported as far as it reads, which is what Python's "incomplete escape"
 * message shows.
 */
static void rxHex(RxCompiler *compiler, size_t count, char kind, size_t escapeOffset, uint32_t *result)
{
    uint32_t value = 0;
    size_t digits = 0;
    while (digits < count && compiler->offset + digits < compiler->size) {
        int digit = bsHexValue(compiler->pattern[compiler->offset + digits]);
        if (digit < 0) {
            break;
        }
        value = (value << 4) | (uint32_t) digit;
        digits++;
    }
    if (digits != count) {
        rxError(compiler, escapeOffset, "incomplete escape \\%c%.*s", kind, (int) digits,
                compiler->pattern + compiler->offset);
        return;
    }
    compiler->offset += count;
    *result = value;
}


/*
 * Accumulate the decimal digits at the offset into "*count"; returns how many there were. The count
 * saturates at INT_MAX: JavaScript accepts any count, and a count that large never matches anyway;
 * CPython raises an OverflowError the Python implementation does not catch.
 */
static size_t rxDigits(RxCompiler *compiler, int *count)
{
    size_t digits = 0;
    *count = 0;
    while (compiler->offset < compiler->size && compiler->pattern[compiler->offset] >= '0' &&
           compiler->pattern[compiler->offset] <= '9') {
        int digit = compiler->pattern[compiler->offset] - '0';
        *count = *count > (INT_MAX - 9) / 10 ? INT_MAX : *count * 10 + digit;
        compiler->offset++;
        digits++;
    }
    return digits;
}


/* Step past the backslash at the offset: an escape needs a character after it */
static bool rxEscapeBegin(RxCompiler *compiler)
{
    size_t escapeOffset = compiler->offset++;
    if (compiler->offset >= compiler->size) {
        rxError(compiler, escapeOffset, "bad escape (end of pattern)");
        return false;
    }
    return true;
}


/*
 * Parse an escape sequence. Returns the predefined class flag, or zero for a literal code point,
 * which is stored in "*literal". On failure, sets compiler->failed, which the caller checks.
 */
static unsigned rxEscape(RxCompiler *compiler, uint32_t *literal)
{
    size_t escapeOffset = compiler->offset - 1;
    char ch = compiler->pattern[compiler->offset++];
    switch (ch) {
    case 'd':
        return RX_CLASS_DIGIT;
    case 'D':
        return RX_CLASS_NOTDIGIT;
    case 'w':
        return RX_CLASS_WORD;
    case 'W':
        return RX_CLASS_NOTWORD;
    case 's':
        return RX_CLASS_SPACE;
    case 'S':
        return RX_CLASS_NOTSPACE;
    case 'n':
        *literal = '\n';
        return 0;
    case 'r':
        *literal = '\r';
        return 0;
    case 't':
        *literal = '\t';
        return 0;
    case 'f':
        *literal = '\f';
        return 0;
    case 'v':
        *literal = 0x0B;
        return 0;
    case '0':
        *literal = rxOctal(compiler, ch);
        return 0;
    case 'x':
    case 'u':
        rxHex(compiler, ch == 'x' ? 2 : 4, ch, escapeOffset, literal);
        return 0;
    case 'c':
        /* A control escape, "\cA"; before anything but a letter, the "\" is itself the literal */
        if (compiler->offset < compiler->size &&
            ((compiler->pattern[compiler->offset] | 0x20) >= 'a' && (compiler->pattern[compiler->offset] | 0x20) <= 'z')) {
            *literal = (uint32_t) (compiler->pattern[compiler->offset++] & 0x1F);
            return 0;
        }
        compiler->offset--;
        *literal = '\\';
        return 0;
    default:
        *literal = (uint32_t) (unsigned char) ch;
        return 0;
    }
}


static RxNode *rxParseAlternation(RxCompiler *compiler);
static void rxClassFinish(RxClass *cls, unsigned flags);


/*
 * Parse one bound of a character class range: a code point, into "*code", or a predefined class
 * escape, into "*classes". "\\b" is a backspace inside a class. False on a compile error.
 */
static bool rxClassBound(RxCompiler *compiler, uint32_t *code, unsigned *classes)
{
    *classes = 0;
    if (compiler->pattern[compiler->offset] == '\\') {
        if (!rxEscapeBegin(compiler)) {
            return false;
        }
        /* A backspace, or an octal escape - a backreference has no meaning in a class */
        char ch = compiler->pattern[compiler->offset];
        if (ch == 'b' || (ch >= '1' && ch <= '7')) {
            compiler->offset++;
            *code = ch == 'b' ? '\b' : rxOctal(compiler, ch);
            return true;
        }
        *classes = rxEscape(compiler, code);
        return !compiler->failed;
    }
    size_t codeSize;
    *code = bsUTF8Decode(compiler->pattern, compiler->size, compiler->offset, &codeSize);
    compiler->offset += codeSize;
    return true;
}


static RxNode *rxParseClass(RxCompiler *compiler, size_t classOffset)
{
    RxNode *node = rxNodeNew(compiler, RX_CLASS);
    if (compiler->offset < compiler->size && compiler->pattern[compiler->offset] == '^') {
        node->u.cls.negate = true;
        compiler->offset++;
    }

    while (compiler->offset < compiler->size) {
        char ch = compiler->pattern[compiler->offset];
        if (ch == ']') {
            /* An empty class - "[]" - matches nothing */
            compiler->offset++;
            rxClassFinish(&node->u.cls, compiler->flags);
            return node;
        }

        /* The range's low bound */
        size_t lowOffset = compiler->offset;
        uint32_t lo = 0;
        unsigned lowClasses;
        if (!rxClassBound(compiler, &lo, &lowClasses)) {
            return NULL;
        }

        /* The optional range's high bound - a class escape is neither bound of a range */
        size_t lowEnd = compiler->offset;
        uint32_t hi = lo;
        unsigned classes = 0;
        if (compiler->offset + 1 < compiler->size && compiler->pattern[compiler->offset] == '-' &&
            compiler->pattern[compiler->offset + 1] != ']') {
            compiler->offset++;
            size_t highOffset = compiler->offset;
            if (!rxClassBound(compiler, &hi, &classes)) {
                return NULL;
            }
            if (lowClasses != 0 || classes != 0 || hi < lo) {
                /* CPython shows each bound's first token - two characters of an escape - and counts
                 * the position back from the range's end */
                size_t lowSize = lowEnd - lowOffset;
                size_t highSize = compiler->offset - highOffset;
                lowSize = compiler->pattern[lowOffset] == '\\' && lowSize > 2 ? 2 : lowSize;
                highSize = compiler->pattern[highOffset] == '\\' && highSize > 2 ? 2 : highSize;
                rxError(compiler, compiler->offset - (lowSize + 1 + highSize), "bad character range %.*s-%.*s",
                        (int) lowSize, compiler->pattern + lowOffset, (int) highSize, compiler->pattern + highOffset);
                return NULL;
            }
        }
        if (lowClasses != 0) {
            node->u.cls.classes |= lowClasses;
        } else {
            rxClassRange(node, lo, hi);
        }
    }

    rxError(compiler, classOffset, "unterminated character set");
    return NULL;
}


static bool rxParseName(RxCompiler *compiler, BSValue *name)
{
    size_t begin = compiler->offset;
    while (compiler->offset < compiler->size && compiler->pattern[compiler->offset] != '>') {
        char ch = compiler->pattern[compiler->offset];
        if (!rxIsWordCode((unsigned char) ch) || (compiler->offset == begin && ch >= '0' && ch <= '9')) {
            return false;
        }
        compiler->offset++;
    }
    if (compiler->offset >= compiler->size || compiler->offset == begin) {
        return false;
    }
    *name = bsStringIntern(compiler->pattern + begin, compiler->offset - begin);
    compiler->offset++;
    return true;
}


/* Whether two groups, by their alternation paths, could both take part in one match */
static bool rxMightBothParticipate(const struct RxPathEntry *a, size_t aCount, const struct RxPathEntry *b, size_t bCount)
{
    for (size_t ix = 0; ix < aCount && ix < bCount; ix++) {
        if (a[ix].alternation != b[ix].alternation || a[ix].branch != b[ix].branch) {
            /* Different branches of one alternation exclude each other; different alternations do not */
            return a[ix].alternation != b[ix].alternation;
        }
    }
    return true;
}


/* Record a named group's alternation path, rejecting a name it shares with a group it could match alongside */
static bool rxGroupNameRegister(RxCompiler *compiler, size_t group, size_t nameOffset)
{
    for (size_t other = 1; other < group; other++) {
        if (compiler->groupNames[other].type == BS_STRING &&
            bsValueCompare(compiler->groupNames[other], compiler->groupNames[group]) == 0 &&
            rxMightBothParticipate(compiler->groupPaths[other].entries, compiler->groupPaths[other].count,
                                   compiler->path, compiler->pathCount)) {
            rxError(compiler, nameOffset, "redefinition of group name '%s' as group %zu; was group %zu",
                    bsStringData(compiler->groupNames[group]), group, other);
            return false;
        }
    }
    size_t size = compiler->pathCount * sizeof(struct RxPathEntry);
    compiler->groupPaths[group].entries = memcpy(bsAlloc(size), compiler->path, size);
    compiler->groupPaths[group].count = compiler->pathCount;
    return true;
}


static RxNode *rxParseAtom(RxCompiler *compiler)
{
    char ch = compiler->pattern[compiler->offset];

    /* Group, non-capturing group, named group, or lookaround */
    if (ch == '(') {
        size_t groupOffset = compiler->offset;
        compiler->offset++;
        RxKind kind = RX_GROUP; /* RX_ALT for a non-capturing group, which is its alternation node */
        bool negate = false;
        BSValue name = bsNull();
        size_t nameOffset = 0;
        if (compiler->offset < compiler->size && compiler->pattern[compiler->offset] == '?') {
            size_t extensionOffset = compiler->offset;
            compiler->offset++;
            if (compiler->offset >= compiler->size) {
                rxError(compiler, compiler->offset, "unexpected end of pattern");
                return NULL;
            }
            size_t extensionKindOffset = compiler->offset;
            char extension = compiler->pattern[compiler->offset++];
            if (extension == ':') {
                kind = RX_ALT;
            } else if (extension == '=' || extension == '!') {
                kind = RX_LOOKAHEAD;
                negate = (extension == '!');
            } else if (extension == '<' && compiler->offset < compiler->size &&
                       (compiler->pattern[compiler->offset] == '=' ||
                        compiler->pattern[compiler->offset] == '!')) {
                kind = RX_LOOKBEHIND;
                negate = (compiler->pattern[compiler->offset] == '!');
                compiler->offset++;
            } else if (extension == '<') {
                if (compiler->offset >= compiler->size) {
                    rxError(compiler, compiler->offset, "unexpected end of pattern");
                    return NULL;
                }
                nameOffset = compiler->offset;
                if (!rxParseName(compiler, &name)) {
                    /* A name of word characters that begins with a digit is one Python's port translates,
                       so its "re" reports the name; any other failure is an unknown extension */
                    size_t nameEnd = nameOffset;
                    while (nameEnd < compiler->size && rxIsWordCode((unsigned char) compiler->pattern[nameEnd])) {
                        nameEnd++;
                    }
                    if (nameEnd > nameOffset && nameEnd < compiler->size && compiler->pattern[nameEnd] == '>') {
                        rxError(compiler, nameOffset, "bad character in group name '%.*s'",
                                (int) (nameEnd - nameOffset), compiler->pattern + nameOffset);
                    } else {
                        rxError(compiler, extensionOffset, "unknown extension ?<%.*s",
                                (int) rxTokenSize(compiler, nameOffset), compiler->pattern + nameOffset);
                    }
                    return NULL;
                }
            } else {
                rxError(compiler, extensionOffset, "unknown extension ?%.*s",
                        (int) rxTokenSize(compiler, extensionKindOffset), compiler->pattern + extensionKindOffset);
                return NULL;
            }
        }

        size_t group = 0;
        if (kind == RX_GROUP) {
            if (compiler->regex->groupCount >= BS_REGEX_GROUPS_MAX) {
                bsRelease(name);
                rxError(compiler, groupOffset, "sorry, but this version only supports %d groups",
                        BS_REGEX_GROUPS_MAX - 1);
                return NULL;
            }
            group = compiler->regex->groupCount++;
            compiler->groupNames[group] = name;
            if (name.type == BS_STRING && !rxGroupNameRegister(compiler, group, nameOffset)) {
                return NULL;
            }
        }

        RxNode *sub = rxParseAlternation(compiler);
        if (sub == NULL) {
            return NULL;
        }
        if (compiler->offset >= compiler->size || compiler->pattern[compiler->offset] != ')') {
            rxError(compiler, groupOffset, "missing ), unterminated subpattern");
            return NULL;
        }
        compiler->offset++;

        if (kind == RX_ALT) {
            /* A non-capturing group is its alternation node - which always has a free "next" */
            return sub;
        }
        RxNode *node = rxNodeNew(compiler, kind);
        if (kind == RX_GROUP) {
            node->u.group.sub = sub;
            node->u.group.group = group;
        } else {
            node->u.look.sub = sub;
            node->u.look.negate = negate;
        }
        return node;
    }

    if (ch == '[') {
        size_t classOffset = compiler->offset;
        compiler->offset++;
        return rxParseClass(compiler, classOffset);
    }

    if (ch == '.' || ch == '^' || ch == '$') {
        compiler->offset++;
        return rxNodeNew(compiler, ch == '.' ? RX_ANY : (ch == '^' ? RX_BOL : RX_EOL));
    }

    if (ch == '\\') {
        size_t escapeOffset = compiler->offset;
        if (!rxEscapeBegin(compiler)) {
            return NULL;
        }
        char escape = compiler->pattern[compiler->offset];

        /* Word boundaries */
        if (escape == 'b' || escape == 'B') {
            compiler->offset++;
            return rxNodeNew(compiler, escape == 'b' ? RX_WORD_BOUNDARY : RX_NOT_WORD_BOUNDARY);
        }

        /* Three octal digits are an octal escape, as both references read them */
        const char *digits = compiler->pattern + compiler->offset;
        if (escape >= '1' && escape <= '7' && compiler->offset + 2 < compiler->size &&
            digits[1] >= '0' && digits[1] <= '7' && digits[2] >= '0' && digits[2] <= '7') {
            uint32_t value = (uint32_t) (escape - '0') * 64 + (uint32_t) (digits[1] - '0') * 8 + (uint32_t) (digits[2] - '0');
            if (value > 0377) {
                rxError(compiler, escapeOffset, "octal escape value \\%.3s outside of range 0-0o377", digits);
                return NULL;
            }
            compiler->offset += 3;
            return rxCharNode(compiler, value);
        }

        /* A numbered backreference */
        if (escape >= '1' && escape <= '9') {
            size_t digitOffset = compiler->offset;
            int group;
            rxDigits(compiler, &group);
            if (group >= BS_REGEX_GROUPS_MAX) {
                rxError(compiler, digitOffset, "invalid group reference %d", group);
                return NULL;
            }
            BS_GROW(compiler->backrefs, compiler->backrefCount, compiler->backrefCap, 8);
            compiler->backrefs[compiler->backrefCount++] = (struct RxBackref) {group, digitOffset};
            RxNode *node = rxNodeNew(compiler, RX_BACKREF);
            node->u.backref.group = (size_t) group;
            return node;
        }

        /* A named backreference */
        if (escape == 'k') {
            bool bracket = compiler->offset + 1 < compiler->size && compiler->pattern[compiler->offset + 1] == '<';
            compiler->offset += 2;
            size_t nameOffset = compiler->offset;
            BSValue name;
            if (!bracket || !rxParseName(compiler, &name)) {
                rxError(compiler, escapeOffset, "bad escape \\k");
                return NULL;
            }
            /* The name resolves once the pattern is parsed - its group may follow, as in JavaScript */
            RxNode *node = rxNodeNew(compiler, RX_BACKREF);
            node->u.backref.name = name;
            BS_GROW(compiler->namedRefs, compiler->namedRefCount, compiler->namedRefCap, 4);
            compiler->namedRefs[compiler->namedRefCount].node = node;
            compiler->namedRefs[compiler->namedRefCount].offset = nameOffset;
            compiler->namedRefCount++;
            return node;
        }

        uint32_t literal = 0;
        unsigned classes = rxEscape(compiler, &literal);
        if (compiler->failed) {
            return NULL;
        }
        if (classes != 0) {
            RxNode *node = rxNodeNew(compiler, RX_CLASS);
            node->u.cls.classes = classes;
            rxClassFinish(&node->u.cls, compiler->flags);
            return node;
        }
        return rxCharNode(compiler, literal);
    }

    /* A literal code point */
    size_t codeSize;
    uint32_t literal = bsUTF8Decode(compiler->pattern, compiler->size, compiler->offset, &codeSize);
    compiler->offset += codeSize;
    return rxCharNode(compiler, literal);
}


/*
 * Match a quantifier, consuming it on success
 *
 * "*", "+", "?", and the counted forms "{n}", "{n,}", "{n,m}" - a "{" that is not a counted
 * quantifier is a literal, as it is in JavaScript. "*max" is -1 for unbounded, and "*digitOffset"
 * is where a counted quantifier's minimum digits start.
 */
static bool rxMatchQuantifier(RxCompiler *compiler, int *min, int *max, size_t *digitOffset)
{
    if (compiler->offset >= compiler->size) {
        return false;
    }
    *digitOffset = compiler->offset;
    char quantifier = compiler->pattern[compiler->offset];
    if (quantifier == '*' || quantifier == '+' || quantifier == '?') {
        *min = (quantifier == '+' ? 1 : 0);
        *max = (quantifier == '?' ? 1 : -1);
        compiler->offset++;
        return true;
    }
    if (quantifier != '{') {
        return false;
    }

    size_t save = compiler->offset;
    compiler->offset++;
    *digitOffset = compiler->offset;
    if (rxDigits(compiler, min) == 0) {
        compiler->offset = save;
        return false;
    }
    *max = *min;
    if (compiler->offset < compiler->size && compiler->pattern[compiler->offset] == ',') {
        compiler->offset++;
        if (rxDigits(compiler, max) == 0) {
            *max = -1;
        }
    }
    if (compiler->offset >= compiler->size || compiler->pattern[compiler->offset] != '}') {
        compiler->offset = save;
        return false;
    }
    compiler->offset++;
    return true;
}


static RxNode *rxParseSequence(RxCompiler *compiler)
{
    RxNode *head = NULL;
    RxNode **tail = &head;
    RxNode *last = NULL;

    while (compiler->offset < compiler->size) {
        char ch = compiler->pattern[compiler->offset];
        if (ch == '|' || ch == ')') {
            break;
        }

        /* A quantifier with no atom to repeat, or one that would repeat a repeat */
        size_t quantifierOffset = compiler->offset;
        int min;
        int max;
        size_t digitOffset;
        if (rxMatchQuantifier(compiler, &min, &max, &digitOffset)) {
            rxError(compiler, quantifierOffset,
                    last != NULL && last->kind == RX_REPEAT ? "multiple repeat" : "nothing to repeat");
            return NULL;
        }

        size_t groupFirst = compiler->regex->groupCount;
        RxNode *atom = rxParseAtom(compiler);
        if (atom == NULL) {
            return NULL;
        }

        /* An optional quantifier - an assertion has nothing to repeat */
        size_t repeatOffset = compiler->offset;
        if (rxMatchQuantifier(compiler, &min, &max, &digitOffset)) {
            if (atom->kind == RX_BOL || atom->kind == RX_EOL || atom->kind == RX_WORD_BOUNDARY ||
                atom->kind == RX_NOT_WORD_BOUNDARY || atom->kind == RX_LOOKAHEAD || atom->kind == RX_LOOKBEHIND) {
                rxError(compiler, repeatOffset, "nothing to repeat");
                return NULL;
            }
            if (max >= 0 && max < min) {
                rxError(compiler, digitOffset, "min repeat greater than max repeat");
                return NULL;
            }

            bool greedy = true;
            if (compiler->offset < compiler->size && compiler->pattern[compiler->offset] == '?') {
                greedy = false;
                compiler->offset++;
            }

            RxNode *repeat = rxNodeNew(compiler, RX_REPEAT);
            repeat->u.repeat.sub = atom;
            repeat->u.repeat.min = min;
            repeat->u.repeat.max = max;
            repeat->u.repeat.greedy = greedy;
            repeat->u.repeat.groupFirst = (uint32_t) groupFirst;
            repeat->u.repeat.groupEnd = (uint32_t) compiler->regex->groupCount;
            atom = repeat;
        }

        *tail = atom;
        tail = &atom->next;
        last = atom;
    }

    return head;
}

static RxNode *rxParseAlternation(RxCompiler *compiler)
{
    RxNode **branches = NULL;
    size_t count = 0;
    size_t capacity = 0;

    /* This alternation joins the path, with its current branch */
    BS_GROW(compiler->path, compiler->pathCount, compiler->pathCap, 16);
    compiler->path[compiler->pathCount].alternation = compiler->alternationCount++;
    compiler->path[compiler->pathCount].branch = 0;
    compiler->pathCount++;
    while (true) {
        RxNode *branch = rxParseSequence(compiler);
        if (compiler->failed) {
            free(branches);
            return NULL;
        }
        BS_GROW(branches, count, capacity, 8);
        branches[count++] = branch;
        if (compiler->offset >= compiler->size || compiler->pattern[compiler->offset] != '|') {
            break;
        }
        compiler->offset++;
        compiler->path[compiler->pathCount - 1].branch++;
    }
    compiler->pathCount--;

    RxNode *alt = rxNodeNew(compiler, RX_ALT);
    alt->u.alt.branches = branches;
    alt->u.alt.count = count;
    return alt;
}


/*
 * Add a code point to a first set - and, matching case-insensitively, every code point its
 * canonical form matches
 */
static void rxFirstAddCode(RxFirstSet *set, unsigned flags, uint32_t code)
{
    uint32_t members[BS_CANON_MEMBERS];
    size_t count = 1;
    members[0] = code;
    if ((flags & BS_REGEX_IGNORECASE) != 0) {
        count = bsUnicodeCanonMembers(rxCanon(code), members);
    }
    for (size_t ix = 0; ix < count; ix++) {
        if (members[ix] >= 256) {
            set->high = true;
        } else {
            set->bits[members[ix] >> 6] |= (uint64_t) 1 << (members[ix] & 63);
        }
    }
}


/*
 * Add the code points that can begin a match of a node chain to "set". Returns true if the chain
 * can match the empty string, in which case the set does not constrain the match position.
 */
static bool rxFirstSet(const RxNode *node, unsigned flags, RxFirstSet *set)
{
    for (; node != NULL; node = node->next) {
        switch (node->kind) {
        case RX_CHAR:
            rxFirstAddCode(set, flags, node->u.ch);
            return false;

        case RX_CLASS: {
            /*
             * The class's ASCII members from its finished membership table - negation and folding
             * applied - and, for a class that can match past ASCII, every code point above: a
             * negated or folded class, a predefined class with such members, or a range reaching them
             */
            const RxClass *cls = &node->u.cls;
            for (size_t ix = 0; ix < 16; ix++) {
                set->bits[ix >> 3] |= (uint64_t) cls->ascii[ix] << (8 * (ix & 7));
            }
            bool wide = cls->negate || cls->fold ||
                (cls->classes & (RX_CLASS_NOTDIGIT | RX_CLASS_NOTWORD | RX_CLASS_SPACE | RX_CLASS_NOTSPACE)) != 0;
            for (size_t ix = 0; !wide && ix < cls->rangeCount; ix++) {
                wide = cls->ranges[ix * 2 + 1] >= 128;
            }
            if (wide) {
                set->bits[2] = set->bits[3] = ~(uint64_t) 0;
                set->high = true;
            }
            return false;
        }

        case RX_ALT: {
            bool nullable = false;
            for (size_t ix = 0; ix < node->u.alt.count; ix++) {
                nullable = rxFirstSet(node->u.alt.branches[ix], flags, set) || nullable;
            }
            if (!nullable) {
                return false;
            }
            break;
        }

        case RX_GROUP:
            if (!rxFirstSet(node->u.group.sub, flags, set)) {
                return false;
            }
            break;

        case RX_REPEAT:
            if (!rxFirstSet(node->u.repeat.sub, flags, set) && node->u.repeat.min > 0) {
                return false;
            }
            break;

        case RX_BOL:
        case RX_EOL:
        case RX_WORD_BOUNDARY:
        case RX_NOT_WORD_BOUNDARY:
        case RX_LOOKAHEAD:
        case RX_LOOKBEHIND:
            /* Zero-width - the match still begins at whatever follows */
            break;

        default:
            /* RX_ANY and RX_BACKREF - not worth enumerating */
            set->any = true;
            return false;
        }
    }
    return true;
}


/* A node chain's first set - unusable if the chain can match the empty string */
static void rxFirstCompute(const RxNode *node, unsigned flags, RxFirstSet *set)
{
    memset(set, 0, sizeof(*set));
    if (rxFirstSet(node, flags, set)) {
        set->any = true;
    }
}


/* Free the node tree - once the program is compiled it is not needed, and on a failed compile */
static void rxChunksFree(RxCompiler *compiler)
{
    RxNodeChunk *chunk = compiler->chunks;
    while (chunk != NULL) {
        for (size_t ix = 0; ix < chunk->used; ix++) {
            RxNode *node = &chunk->nodes[ix];
            if (node->kind == RX_CLASS) {
                free(node->u.cls.ranges);
            } else if (node->kind == RX_ALT) {
                free(node->u.alt.branches);
            }
        }
        RxNodeChunk *next = chunk->next;
        free(chunk);
        chunk = next;
    }
}


static void bsRegexFree(BSRegex *regex)
{
    rxProgramFree(regex);
    if (regex->groupNames != NULL) {
        for (size_t ix = 0; ix < regex->groupCount; ix++) {
            bsRelease(regex->groupNames[ix]);
        }
        free(regex->groupNames);
    }
    free(regex);
}


/* Free the compiler's own lists - the numbered and named references, the group paths */
static void rxCompilerFree(RxCompiler *compiler)
{
    free(compiler->backrefs);
    for (size_t ix = 0; ix < compiler->namedRefCount; ix++) {
        bsRelease(compiler->namedRefs[ix].node->u.backref.name);
        free(compiler->namedRefs[ix].node->u.backref.groups);
    }
    free(compiler->namedRefs);
    for (size_t ix = 0; ix < compiler->regex->groupCount; ix++) {
        free(compiler->groupPaths[ix].entries);
    }
    free(compiler->path);
}


BSValue bsRegexNew(const char *pattern, size_t patternSize, unsigned flags, char *error,
                   size_t errorSize)
{
    if (error != NULL && errorSize != 0) {
        error[0] = '\0';
    }
    BSRegex *regex = bsAlloc(sizeof(BSRegex));
    memset(regex, 0, sizeof(*regex));
    regex->refcount = 1;
    regex->groupCount = 1;

    RxCompiler compiler;
    memset(&compiler, 0, sizeof(compiler));
    compiler.pattern = pattern;
    compiler.size = patternSize;
    compiler.flags = flags;
    compiler.regex = regex;
    compiler.error = error;
    compiler.errorSize = errorSize;
    compiler.root = rxParseAlternation(&compiler);

    /* A numbered backreference may precede its group, as in JavaScript, but the group must exist */
    for (size_t ix = 0; !compiler.failed && ix < compiler.backrefCount; ix++) {
        if ((size_t) compiler.backrefs[ix].group >= regex->groupCount) {
            rxError(&compiler, compiler.backrefs[ix].offset, "invalid group reference %d", compiler.backrefs[ix].group);
        }
    }

    /* A named backreference may precede its group too, and a name shared across the branches of an
       alternation refers to whichever group took part */
    for (size_t ix = 0; !compiler.failed && ix < compiler.namedRefCount; ix++) {
        RxNode *node = compiler.namedRefs[ix].node;
        uint32_t groups[BS_REGEX_GROUPS_MAX];
        size_t count = 0;
        for (size_t group = 1; group < regex->groupCount; group++) {
            if (compiler.groupNames[group].type == BS_STRING &&
                bsValueCompare(compiler.groupNames[group], node->u.backref.name) == 0) {
                groups[count++] = (uint32_t) group;
            }
        }
        if (count == 0) {
            rxError(&compiler, compiler.namedRefs[ix].offset, "unknown group name '%s'", bsStringData(node->u.backref.name));
        } else {
            node->u.backref.group = groups[0];
            if (count > 1) {
                node->u.backref.groups = memcpy(bsAlloc(count * sizeof(uint32_t)), groups, count * sizeof(uint32_t));
                node->u.backref.count = count;
            }
        }
    }
    if (!compiler.failed && compiler.offset != patternSize) {
        rxError(&compiler, compiler.offset, "unbalanced parenthesis");
    }
    if (compiler.failed) {
        for (size_t ix = 0; ix < regex->groupCount; ix++) {
            bsRelease(compiler.groupNames[ix]);
        }
        rxCompilerFree(&compiler);
        rxChunksFree(&compiler);
        bsRegexFree(regex);
        return bsNull();
    }

    /*
     * A pattern whose every alternative begins with "^" can only match at the search start, so
     * the scan over later positions is skipped. Multi-line patterns still scan, since "^" also
     * matches after a newline. The parser's patterns are all anchored, so this is the
     * difference between a linear and a quadratic scan over every line it parses.
     */
    if ((flags & BS_REGEX_MULTILINE) == 0) {
        regex->anchored = true;
        for (size_t ix = 0; ix < compiler.root->u.alt.count; ix++) {
            const RxNode *branch = compiler.root->u.alt.branches[ix];
            if (branch == NULL || branch->kind != RX_BOL) {
                regex->anchored = false;
                break;
            }
        }
    }

    /* The set of code points a match can begin with, for the search scan - by byte for an ASCII subject */
    rxFirstCompute(compiler.root, flags, &regex->first);
    regex->firstByte = -1;
    if (!regex->first.any) {
        int firstCount = 0;
        for (unsigned word = 0; word < 4; word++) {
            for (uint64_t bits = regex->first.bits[word]; bits != 0; bits &= bits - 1) {
                unsigned code = word * 64 + rxLowestBit64(bits);
                regex->firstBytes[code] = 1;
                regex->firstByte = firstCount++ == 0 ? (int) code : -1;
            }
        }
    }

    rxEmitProgram(&compiler);
    rxCompilerFree(&compiler);
    rxChunksFree(&compiler);

    /* Keep named-group strings only; unnamed patterns store no name array */
    bool named = false;
    for (size_t ix = 0; ix < regex->groupCount; ix++) {
        if (compiler.groupNames[ix].type == BS_STRING) {
            named = true;
            break;
        }
    }
    if (named) {
        regex->groupNames = bsAlloc(regex->groupCount * sizeof(BSValue));
        memcpy(regex->groupNames, compiler.groupNames, regex->groupCount * sizeof(BSValue));
        regex->uniqueNames = true;
        for (size_t ix = 0; ix < regex->groupCount && regex->uniqueNames; ix++) {
            for (size_t jx = 0; jx < ix; jx++) {
                if (regex->groupNames[ix].type == BS_STRING && regex->groupNames[jx].type == BS_STRING &&
                    bsValueCompare(regex->groupNames[ix], regex->groupNames[jx]) == 0) {
                    regex->uniqueNames = false;
                }
            }
        }
    }

    BSValue value;
    value.type = BS_REGEX;
    value.u.regex = regex;
    return value;
}


/* Called once the shared refcount reaches zero - see bsReleaseInline */
void bsRegexDestroy(BSValue value)
{
    bsRegexFree(value.u.regex);
}


bool bsRegexGroupNamesUnique(BSValue regex)
{
    return regex.u.regex->uniqueNames;
}


bool bsRegexGroupsNamed(BSValue regex)
{
    return regex.u.regex->groupNames != NULL;
}


BSValue bsRegexGroupNameValue(BSValue regex, size_t group)
{
    BSValue *names = regex.u.regex->groupNames;
    if (names == NULL || group >= regex.u.regex->groupCount) {
        return bsNull();
    }
    return names[group];
}


/*
 * Match
 */


/*
 * The program matcher
 *
 * A pattern compiles to a linear program that one loop runs with an explicit backtrack stack, so
 * matching costs no C recursion beyond one call per lookaround body: a node's alternatives, a
 * repeat's iterations, and a simple repeat's give-back are backtrack entries, and every capture
 * and repeat-counter write is on the trail, which a backtrack unwinds to the entry's mark.
 */
typedef enum {
    RXI_CHAR,          /* operand: the code point; the first five opcodes are also the encoded atom kinds */
    RXI_CHAR_FOLD,     /* operand: the folded code point */
    RXI_ANY,
    RXI_ANY_ALL,
    RXI_CLASS,         /* operand: the class */
    RXI_ALT,           /* operand: the alternation */
    RXI_JMP,           /* a: the target */
    RXI_GROUP_BEGIN,   /* a: the group */
    RXI_GROUP_END,
    RXI_BOL,
    RXI_BOL_ML,
    RXI_EOL,
    RXI_EOL_ML,
    RXI_WB,
    RXI_NWB,
    RXI_BACKREF,       /* a: the group, or with b groups the table index of the ones sharing a name; aux: fold, backward */
    RXI_LOOK_ATOM,     /* aux, operand: the atom; aux: negate and behind */
    RXI_LOOK,          /* a: the body's program - emitted backward for a lookbehind; b: the continuation; aux: negate */
    RXI_REPEAT_SIMPLE, /* aux, operand: the atom; a: min; b: max; aux: greedy; the continuation follows */
    RXI_REPEAT_ENTER,  /* slot: the counter to reset */
    RXI_REPEAT_LOOP,   /* slot; a: the exit; b: max; operand: min; aux: greedy; the body follows */
    RXI_REPEAT_NEXT,   /* slot; a: the loop; b: the exit; operand: min */
    RXI_MATCH,         /* the pattern matched */
    /*
     * A lookbehind's body matches right to left, so it is emitted last node first with these forms
     * of the instructions that consume the subject, each matching the code points before "pos"
     */
    RXI_ATOM_BACK,     /* aux, operand: the atom */
    RXI_REPEAT_SIMPLE_BACK,
    RXI_GROUP_END_BACK,   /* a: the group - met first, going backward */
    RXI_GROUP_BEGIN_BACK
} RxOp;

struct RxInst {
    uint8_t op;
    uint8_t aux;      /* an atom's kind and flags, a lookaround's negation, a repeat's greed */
    uint16_t slot;    /* the repeat counter slot */
    uint32_t a;
    uint32_t b;
    uint32_t operand; /* a class or alternation index; an atom's code point; a repeat's minimum */
};

/* An encoded single-code-point atom, in an instruction's aux (the kind - the opcode that matches it once) and operand */
#define RX_ATOM_KIND      0x07
#define RX_ATOM_FLAG      0x08 /* a simple repeat's greed; a lookaround atom's negation */
#define RX_LOOK_BEHIND    0x10

/* A backreference's aux */
#define RX_BACKREF_FOLD   0x01
#define RX_BACKREF_BACK   0x02

/* A repeat bound with no maximum */
#define RX_UNBOUNDED 0xFFFFFFFFu

/* An alternation, as the program keeps it */
typedef struct RxAlt {
    uint32_t count;
    uint32_t *branchPcs;    /* each alternative's program */
    RxFirstSet *firsts;     /* per-alternative first sets, or NULL if none is usable */
    RxAltIndex *index;      /* the alternatives by first code point, for a wide alternation */
} RxAlt;



/* A backtrack entry - what to try next when the current path fails */
typedef enum {
    RX_BT_SPLIT,            /* resume at pc, pos */
    RX_BT_REPEAT_BODY,      /* enter the body of the repeat loop at pc */
    RX_BT_ALT_MASK,         /* the alternation at pc; aux: the alternatives still to try */
    RX_BT_ALT_INDEX,        /* the alternation at pc; aux: the next alternative to try */
    RX_BT_GIVEBACK,         /* the simple repeat before pc, greedy: give back to aux */
    RX_BT_LAZY,             /* the simple repeat before pc, lazy: take one more; aux: the start */
    RX_BT_GIVEBACK_BACK,    /* the same two, for a repeat matching backward */
    RX_BT_LAZY_BACK
} RxBtKind;

typedef struct RxBacktrack {
    uint32_t kind;
    uint32_t pc;
    uint32_t pos;
    uint32_t trail;
    uint64_t aux;
} RxBacktrack;

/*
 * The backtrack stack's limit, in entries - a memory guard. A push past it is dropped and the step
 * budget exhausted, so the run gives up at its next choice point or backtrack, the only places the
 * dropped entry could have mattered. Every choice point charges the step budget before it pushes,
 * so the budget trips first; the guard only bounds a run whose simple repeats push more than the
 * budget allows.
 */
#define RX_BACKTRACK_MAX 1000000


/* A counted repeat's state - the iterations taken and the current iteration's start */
typedef struct RxRepeat {
    size_t count;
    size_t start;
} RxRepeat;

/* The capture groups within a counted repeat's body - [first, end) - which every iteration begins unset */
typedef struct RxRepeatGroups {
    uint32_t first;
    uint32_t end;
} RxRepeatGroups;

/* A trail entry restoring a repeat counter rather than a capture - the slot is in the low bits */
#define RX_TRAIL_REPEAT 0x80000000u

/*
 * A trail entry
 *
 * Every capture group and repeat counter write records its previous value, so backtracking - and
 * in particular lookaround, which can write many groups before failing - restores state in time
 * proportional to what actually changed rather than copying the whole capture array.
 */
typedef struct RxTrailEntry {
    uint32_t group;
    BSRegexSpan span; /* for a repeat: begin is the count, end the start */
    bool matched;
} RxTrailEntry;


typedef struct RxState {
    const uint32_t *codes;
    const unsigned char *bytes;
    size_t length;
    BSRegexMatch *match;
    size_t end;
    long steps;
    RxTrailEntry *trail;
    size_t trailCount;
    size_t trailCapacity;
    const RxInst *prog;
    const RxClass *classes;
    const RxAlt *alts;
    RxBacktrack *bt;
    size_t btCount;
    size_t btCapacity;
    RxRepeat *repeats;
    const RxRepeatGroups *repeatGroups;
    const uint32_t *backrefGroups;
} RxState;


/*
 * The thread's match scratch - the trail, the backtrack stack, and the repeat counters - kept
 * from one search to the next at the largest size a search has needed, so a search allocates
 * nothing and carries no stack frame of its own. A search never re-enters the matcher.
 */
typedef struct RxScratch {
    RxTrailEntry *trail;
    size_t trailCapacity;
    RxBacktrack *bt;
    size_t btCapacity;
    RxRepeat *repeats;
    size_t repeatCapacity;
} RxScratch;

static _Thread_local RxScratch bsRxScratch;

#define RX_TRAIL_INITIAL 64
#define RX_BACKTRACK_INITIAL 128


void bsRegexScratchFree(void)
{
    free(bsRxScratch.trail);
    free(bsRxScratch.bt);
    free(bsRxScratch.repeats);
    memset(&bsRxScratch, 0, sizeof(bsRxScratch));
}


static inline uint32_t rxCode(const RxState *state, size_t pos)
{
    return state->codes != NULL ? state->codes[pos] : (uint32_t) state->bytes[pos];
}


/* The next trail entry, to be filled in */
static inline RxTrailEntry *rxTrailNext(RxState *state)
{
    if (state->trailCount == state->trailCapacity) {
        state->trailCapacity *= 2;
        state->trail = bsRealloc(state->trail, state->trailCapacity * sizeof(RxTrailEntry));
    }
    return &state->trail[state->trailCount++];
}


static void rxTrailPush(RxState *state, size_t group)
{
    RxTrailEntry *entry = rxTrailNext(state);
    entry->group = (uint32_t) group;
    entry->span = state->match->groups[group];
    entry->matched = state->match->matched[group];
}


static void rxTrailUnwind(RxState *state, size_t mark)
{
    while (state->trailCount > mark) {
        RxTrailEntry *entry = &state->trail[--state->trailCount];
        if ((entry->group & RX_TRAIL_REPEAT) != 0) {
            uint32_t slot = entry->group & ~RX_TRAIL_REPEAT;
            state->repeats[slot].count = entry->span.begin;
            state->repeats[slot].start = entry->span.end;
        } else {
            state->match->groups[entry->group] = entry->span;
            state->match->matched[entry->group] = entry->matched;
        }
    }
}


static bool rxClassMatchOne(const RxClass *cls, uint32_t ch)
{
    unsigned classes = cls->classes;
    if ((classes & RX_CLASS_DIGIT) != 0 && ch >= '0' && ch <= '9') {
        return true;
    }
    if ((classes & RX_CLASS_NOTDIGIT) != 0 && !(ch >= '0' && ch <= '9')) {
        return true;
    }
    if ((classes & RX_CLASS_WORD) != 0 && rxIsWordCode(ch)) {
        return true;
    }
    if ((classes & RX_CLASS_NOTWORD) != 0 && !rxIsWordCode(ch)) {
        return true;
    }
    if ((classes & RX_CLASS_SPACE) != 0 && bsIsSpaceCode(ch)) {
        return true;
    }
    if ((classes & RX_CLASS_NOTSPACE) != 0 && !bsIsSpaceCode(ch)) {
        return true;
    }
    for (size_t ix = 0; ix < cls->rangeCount; ix++) {
        if (ch >= cls->ranges[ix * 2] && ch <= cls->ranges[ix * 2 + 1]) {
            return true;
        }
    }
    return false;
}


/* Whether a class has a member that a code point matches case-insensitively */
static bool rxClassMatchFold(const RxClass *cls, uint32_t ch)
{
    uint32_t members[BS_CANON_MEMBERS];
    size_t count = bsUnicodeCanonMembers(rxCanon(ch), members);
    for (size_t ix = 0; ix < count; ix++) {
        if (rxClassMatchOne(cls, members[ix])) {
            return true;
        }
    }
    return false;
}


/*
 * Precompute a class's membership for the code points 0 - 127, with the case-insensitivity flag
 * and negation applied, so an ASCII subject tests one bit instead of walking the ranges
 */
static BS_NOINLINE void rxClassFinish(RxClass *cls, unsigned flags)
{
    cls->fold = (flags & BS_REGEX_IGNORECASE) != 0;
    memset(cls->ascii, 0, sizeof(cls->ascii));
    if (cls->fold) {
        /* A folded class matches a code point through its canonical group's members */
        for (uint32_t ch = 0; ch < 128; ch++) {
            if (rxClassMatchFold(cls, ch) != cls->negate) {
                cls->ascii[ch >> 3] |= (uint8_t) (1u << (ch & 7));
            }
        }
        return;
    }

    /* The predefined classes' members below 128, then the ranges' - as bits, then negated as a whole */
    static const uint64_t digits = 0x03FF000000000000u;                 /* 0-9 */
    static const uint64_t spaces = 0x0000000100003E00u;                 /* \t \n \v \f \r and the space */
    static const uint64_t wordLow = 0x03FF000000000000u;                /* 0-9 */
    static const uint64_t wordHigh = 0x07FFFFFE87FFFFFEu;               /* A-Z _ a-z */
    uint64_t bits[2] = {0, 0};
    unsigned classes = cls->classes;
    if ((classes & RX_CLASS_DIGIT) != 0) {
        bits[0] |= digits;
    }
    if ((classes & RX_CLASS_NOTDIGIT) != 0) {
        bits[0] |= ~digits;
        bits[1] = ~(uint64_t) 0;
    }
    if ((classes & RX_CLASS_WORD) != 0) {
        bits[0] |= wordLow;
        bits[1] |= wordHigh;
    }
    if ((classes & RX_CLASS_NOTWORD) != 0) {
        bits[0] |= ~wordLow;
        bits[1] |= ~wordHigh;
    }
    if ((classes & RX_CLASS_SPACE) != 0) {
        bits[0] |= spaces;
    }
    if ((classes & RX_CLASS_NOTSPACE) != 0) {
        bits[0] |= ~spaces;
        bits[1] = ~(uint64_t) 0;
    }
    for (size_t ix = 0; ix < cls->rangeCount; ix++) {
        for (uint32_t ch = cls->ranges[ix * 2]; ch < 128 && ch <= cls->ranges[ix * 2 + 1]; ch++) {
            bits[ch >> 6] |= (uint64_t) 1 << (ch & 63);
        }
    }
    if (cls->negate) {
        bits[0] = ~bits[0];
        bits[1] = ~bits[1];
    }
    for (size_t ix = 0; ix < 8; ix++) {
        cls->ascii[ix] = (uint8_t) (bits[0] >> (8 * ix));
        cls->ascii[8 + ix] = (uint8_t) (bits[1] >> (8 * ix));
    }
}


static inline bool rxClassMatch(const RxClass *cls, uint32_t ch)
{
    if (ch < 128) {
        return (cls->ascii[ch >> 3] >> (ch & 7)) & 1u;
    }
    return (cls->fold ? rxClassMatchFold(cls, ch) : rxClassMatchOne(cls, ch)) != cls->negate;
}


typedef struct RxEmit {
    RxInst *inst;
    size_t count;
    size_t capacity;
    RxClass *classes;
    size_t classCount;
    size_t classCapacity;
    RxAlt *alts;
    size_t altCount;
    size_t altCapacity;
    bool backward;          /* emitting a lookbehind body, which matches right to left */
    RxRepeatGroups *repeatGroups;
    size_t repeatGroupCapacity;
    uint32_t *backrefGroups;
    size_t backrefGroupCount;
    size_t backrefGroupCap;
    uint32_t repeatSlots;
    unsigned flags;
} RxEmit;


static uint32_t rxEmit(RxEmit *e, uint8_t op, uint32_t a, uint32_t b, uint32_t operand, uint8_t aux,
                       uint32_t slot)
{
    BS_GROW(e->inst, e->count, e->capacity, 32);
    RxInst *inst = &e->inst[e->count];
    inst->op = op;
    inst->aux = aux;
    inst->slot = (uint16_t) slot;
    inst->a = a;
    inst->b = b;
    inst->operand = operand;
    return (uint32_t) e->count++;
}


static uint32_t rxEmitOp(RxEmit *e, uint8_t op)
{
    return rxEmit(e, op, 0, 0, 0, 0, 0);
}


/* Move a class node's data into the program's class table. Returns the class index. */
static uint32_t rxEmitClass(RxEmit *e, RxNode *node)
{
    BS_GROW(e->classes, e->classCount, e->classCapacity, 8);
    e->classes[e->classCount] = node->u.cls;
    node->u.cls.ranges = NULL;
    return (uint32_t) e->classCount++;
}


/* Encode a single-code-point node as an atom kind and operand */
static uint32_t rxEmitAtom(RxEmit *e, RxNode *atom, unsigned *kind)
{
    switch (atom->kind) {
    case RX_CHAR:
        *kind = (e->flags & BS_REGEX_IGNORECASE) != 0 ? RXI_CHAR_FOLD : RXI_CHAR;
        return atom->u.ch;
    case RX_ANY:
        *kind = (e->flags & BS_REGEX_DOTALL) != 0 ? RXI_ANY_ALL : RXI_ANY;
        return 0;
    default:
        *kind = RXI_CLASS;
        return rxEmitClass(e, atom);
    }
}


/*
 * Each alternative's first set, so an alternation tries only the alternatives that can start at a
 * position - and, for a wide alternation, an index of the alternatives by first code point
 */
static void rxEmitAltFirsts(RxAlt *alt, const RxNode *node, unsigned flags)
{
    size_t count = node->u.alt.count;
    RxFirstSet *firsts = bsAlloc(count * sizeof(RxFirstSet));
    bool usable = false;
    for (size_t ixBranch = 0; ixBranch < count; ixBranch++) {
        rxFirstCompute(node->u.alt.branches[ixBranch], flags, &firsts[ixBranch]);
        usable = usable || !firsts[ixBranch].any;
    }
    alt->firsts = NULL;
    alt->index = NULL;
    if (!usable) {
        free(firsts);
        return;
    }
    alt->firsts = firsts;
    if (count < RX_ALT_INDEX_MIN || count > RX_ALT_INDEX_MAX) {
        return;
    }
    RxAltIndex *index = bsAlloc(sizeof(RxAltIndex));
    memset(index, 0, sizeof(*index));
    for (size_t ixBranch = 0; ixBranch < count; ixBranch++) {
        const RxFirstSet *first = &firsts[ixBranch];
        uint64_t bit = (uint64_t) 1 << ixBranch;
        if (first->any) {
            index->always |= bit;
            continue;
        }
        if (first->high || first->bits[2] != 0 || first->bits[3] != 0) {
            index->high |= bit;
        }
        for (unsigned word = 0; word < 2; word++) {
            for (uint64_t bits = first->bits[word]; bits != 0; bits &= bits - 1) {
                index->codes[word * 64 + rxLowestBit64(bits)] |= bit;
            }
        }
    }
    alt->index = index;
}


static void rxEmitChain(RxEmit *e, RxNode *node);

/* Emit one node */
static void rxEmitNode(RxEmit *e, RxNode *node)
{
    switch (node->kind) {
    case RX_CHAR:
    case RX_ANY:
    case RX_CLASS: {
        unsigned kind;
        uint32_t operand = rxEmitAtom(e, node, &kind);
        if (e->backward) {
            rxEmit(e, RXI_ATOM_BACK, 0, 0, operand, (uint8_t) kind, 0);
        } else {
            rxEmit(e, (uint8_t) kind, 0, 0, operand, 0, 0);
        }
        break;
    }

    case RX_ALT: {
        size_t count = node->u.alt.count;
        if (count == 1) {
            rxEmitChain(e, node->u.alt.branches[0]);
            break;
        }
        BS_GROW(e->alts, e->altCount, e->altCapacity, 8);
        uint32_t altIndex = (uint32_t) e->altCount++;
        e->alts[altIndex].count = (uint32_t) count;
        e->alts[altIndex].firsts = NULL;
        e->alts[altIndex].index = NULL;
        if (!e->backward) {
            /* A first set describes a forward match; backward, every alternative is tried */
            rxEmitAltFirsts(&e->alts[altIndex], node, e->flags);
        }
        rxEmit(e, RXI_ALT, 0, 0, altIndex, 0, 0);
        uint32_t *pcs = bsAlloc(count * sizeof(uint32_t));
        uint32_t *jumps = bsAlloc(count * sizeof(uint32_t));
        for (size_t ix = 0; ix < count; ix++) {
            pcs[ix] = (uint32_t) e->count;
            rxEmitChain(e, node->u.alt.branches[ix]);
            jumps[ix] = rxEmitOp(e, RXI_JMP);
        }
        for (size_t ix = 0; ix < count; ix++) {
            e->inst[jumps[ix]].a = (uint32_t) e->count;
        }
        free(jumps);
        e->alts[altIndex].branchPcs = pcs;
        break;
    }

    case RX_GROUP:
        rxEmit(e, e->backward ? RXI_GROUP_END_BACK : RXI_GROUP_BEGIN, (uint32_t) node->u.group.group, 0, 0, 0, 0);
        rxEmitChain(e, node->u.group.sub);
        rxEmit(e, e->backward ? RXI_GROUP_BEGIN_BACK : RXI_GROUP_END, (uint32_t) node->u.group.group, 0, 0, 0, 0);
        break;

    case RX_REPEAT: {
        int max = node->u.repeat.max;
        if (max == 0) {
            break;
        }
        uint32_t maxOperand = max < 0 ? RX_UNBOUNDED : (uint32_t) max;
        uint8_t greedy = node->u.repeat.greedy ? RX_ATOM_FLAG : 0;
        if (rxIsSimple(node->u.repeat.sub)) {
            unsigned kind;
            uint32_t operand = rxEmitAtom(e, node->u.repeat.sub, &kind);
            rxEmit(e, e->backward ? RXI_REPEAT_SIMPLE_BACK : RXI_REPEAT_SIMPLE, (uint32_t) node->u.repeat.min,
                   maxOperand, operand, (uint8_t) (kind | greedy), 0);
            break;
        }
        uint32_t slot = e->repeatSlots;
        BS_GROW(e->repeatGroups, e->repeatSlots, e->repeatGroupCapacity, 4);
        e->repeatGroups[e->repeatSlots++] = (RxRepeatGroups) {node->u.repeat.groupFirst, node->u.repeat.groupEnd};
        rxEmit(e, RXI_REPEAT_ENTER, 0, 0, 0, 0, slot);
        uint32_t loop = rxEmit(e, RXI_REPEAT_LOOP, 0, maxOperand, (uint32_t) node->u.repeat.min, greedy, slot);
        rxEmitChain(e, node->u.repeat.sub);
        uint32_t next = rxEmit(e, RXI_REPEAT_NEXT, loop, 0, (uint32_t) node->u.repeat.min, 0, slot);
        e->inst[loop].a = (uint32_t) e->count;
        e->inst[next].b = (uint32_t) e->count;
        break;
    }

    case RX_BOL:
        rxEmitOp(e, (e->flags & BS_REGEX_MULTILINE) != 0 ? RXI_BOL_ML : RXI_BOL);
        break;

    case RX_EOL:
        rxEmitOp(e, (e->flags & BS_REGEX_MULTILINE) != 0 ? RXI_EOL_ML : RXI_EOL);
        break;

    case RX_WORD_BOUNDARY:
        rxEmitOp(e, RXI_WB);
        break;

    case RX_NOT_WORD_BOUNDARY:
        rxEmitOp(e, RXI_NWB);
        break;

    case RX_BACKREF: {
        /* A reference to a shared name indexes the table of its groups, "b" of them */
        uint32_t a = (uint32_t) node->u.backref.group;
        uint32_t b = 0;
        if (node->u.backref.count > 1) {
            a = (uint32_t) e->backrefGroupCount;
            b = (uint32_t) node->u.backref.count;
            for (size_t ix = 0; ix < node->u.backref.count; ix++) {
                BS_GROW(e->backrefGroups, e->backrefGroupCount, e->backrefGroupCap, 8);
                e->backrefGroups[e->backrefGroupCount++] = node->u.backref.groups[ix];
            }
        }
        rxEmit(e, RXI_BACKREF, a, b, 0,
               (uint8_t) (((e->flags & BS_REGEX_IGNORECASE) != 0 ? RX_BACKREF_FOLD : 0) | (e->backward ? RX_BACKREF_BACK : 0)), 0);
        break;
    }

    case RX_LOOKAHEAD:
    case RX_LOOKBEHIND: {
        bool ahead = node->kind == RX_LOOKAHEAD;
        uint8_t negate = node->u.look.negate ? RX_ATOM_FLAG : 0;
        RxNode *atom = rxSimpleAtom(node->u.look.sub);
        if (atom != NULL) {
            unsigned kind;
            uint32_t operand = rxEmitAtom(e, atom, &kind);
            rxEmit(e, RXI_LOOK_ATOM, 0, 0, operand, (uint8_t) (kind | negate | (ahead ? 0 : RX_LOOK_BEHIND)), 0);
            break;
        }
        /* A lookahead's body matches forward from here, even within a lookbehind; a lookbehind's backward */
        uint32_t look = rxEmit(e, RXI_LOOK, 0, 0, 0, negate, 0);
        e->inst[look].a = (uint32_t) e->count;
        bool backward = e->backward;
        e->backward = !ahead;
        rxEmitChain(e, node->u.look.sub);
        e->backward = backward;
        rxEmitOp(e, RXI_MATCH);
        e->inst[look].b = (uint32_t) e->count;
        break;
    }
    }
}


/* Emit a node chain - last node first for a lookbehind body, which matches right to left */
static void rxEmitChain(RxEmit *e, RxNode *node)
{
    if (!e->backward) {
        for (; node != NULL; node = node->next) {
            rxEmitNode(e, node);
        }
        return;
    }
    RxNode **nodes = NULL;
    size_t count = 0;
    size_t capacity = 0;
    for (; node != NULL; node = node->next) {
        BS_GROW(nodes, count, capacity, 8);
        nodes[count++] = node;
    }
    while (count > 0) {
        rxEmitNode(e, nodes[--count]);
    }
    free(nodes);
}


static void rxProgramFree(BSRegex *regex)
{
    for (size_t ix = 0; ix < regex->classCount; ix++) {
        free(regex->classes[ix].ranges);
    }
    free(regex->classes);
    for (size_t ix = 0; ix < regex->altCount; ix++) {
        free(regex->alts[ix].branchPcs);
        free(regex->alts[ix].firsts);
        free(regex->alts[ix].index);
    }
    free(regex->alts);
    free(regex->repeatGroups);
    free(regex->backrefGroups);
    free(regex->prog);
}


static void rxEmitProgram(RxCompiler *compiler)
{
    BSRegex *regex = compiler->regex;
    RxEmit e;
    memset(&e, 0, sizeof(e));
    e.flags = compiler->flags;
    rxEmitChain(&e, compiler->root);
    rxEmitOp(&e, RXI_MATCH);
    regex->prog = bsRealloc(e.inst, e.count * sizeof(RxInst));
    regex->classes = e.classes;
    regex->classCount = e.classCount;
    regex->alts = e.alts;
    regex->altCount = e.altCount;
    regex->repeatGroups = e.repeatGroups;
    regex->backrefGroups = e.backrefGroups;
    regex->repeatCount = e.repeatSlots;
}


static void rxBtPush(RxState *state, uint32_t kind, uint32_t pc, size_t pos, uint64_t aux)
{
    if (state->btCount == state->btCapacity) {
        /* GCOV_EXCL_START - a memory guard the step budget reaches first */
        if (state->btCapacity >= RX_BACKTRACK_MAX) {
            state->steps = RX_STEPS_MAX;
            return;
        }
        /* GCOV_EXCL_STOP */
        state->btCapacity *= 2;
        state->bt = bsRealloc(state->bt, state->btCapacity * sizeof(RxBacktrack));
    }
    RxBacktrack *entry = &state->bt[state->btCount++];
    entry->kind = kind;
    entry->pc = pc;
    entry->pos = (uint32_t) pos;
    entry->trail = (uint32_t) state->trailCount;
    entry->aux = aux;
}


static void rxTrailPushRepeat(RxState *state, uint32_t slot)
{
    RxTrailEntry *entry = rxTrailNext(state);
    entry->group = slot | RX_TRAIL_REPEAT;
    entry->span.begin = state->repeats[slot].count;
    entry->span.end = state->repeats[slot].start;
}


/* Begin an iteration of a counted repeat at "pos": the body's captures start unset, as JavaScript's do */
static void rxRepeatEnter(RxState *state, uint32_t slot, size_t pos)
{
    const RxRepeatGroups *groups = &state->repeatGroups[slot];
    for (uint32_t group = groups->first; group < groups->end; group++) {
        if (state->match->matched[group]) {
            rxTrailPush(state, group);
            state->match->matched[group] = false;
        }
    }
    rxTrailPushRepeat(state, slot);
    state->repeats[slot].start = pos;
    state->repeats[slot].count++;
}


/* Whether an alternative can begin at a position, by its first set */
static inline bool rxAltViable(const RxAlt *alt, size_t ix, bool atEnd, uint32_t code)
{
    const RxFirstSet *firsts = alt->firsts;
    if (firsts == NULL || firsts[ix].any) {
        return true;
    }
    return !atEnd && rxFirstHas(&firsts[ix], code);
}


/*
 * Give back a simple repeat, from "*end" down to "stop", until the literal "ch" that must follow it
 * is found at "*end"; false if no position that far back holds it
 */
static inline bool rxGiveBackTo(const RxState *state, uint32_t ch, size_t stop, size_t *end)
{
    while (!(*end < state->length && rxCode(state, *end) == ch)) {
        if (*end == stop) {
            return false;
        }
        (*end)--;
    }
    return true;
}


/* Whether an encoded single-code-point atom matches at a position */
static inline bool rxAtomAt(const RxState *state, unsigned kind, uint32_t operand, size_t pos)
{
    if (pos >= state->length) {
        return false;
    }
    uint32_t ch = rxCode(state, pos);
    switch (kind) {
    case RXI_CHAR:
        return ch == operand;
    case RXI_CHAR_FOLD:
        return rxCanon(ch) == operand;
    case RXI_ANY:
        return !rxIsLineTerminator(ch);
    case RXI_ANY_ALL:
        return true;
    default:
        return rxClassMatch(&state->classes[operand], ch);
    }
}


/*
 * Run the program from "pc" at "pos" to RXI_MATCH - the pattern, or a lookaround's body. Returns
 * whether the program matched - the pattern's end position is left in state->end - with the
 * backtrack entries the run pushed discarded either way.
 */
static bool rxRun(RxState *state, uint32_t startPc, size_t startPos)
{
    const RxInst *prog = state->prog;
    const size_t length = state->length;
    const size_t btBase = state->btCount;
    const size_t trailBase = state->trailCount;
    uint32_t pc = startPc;
    size_t pos = startPos;

#ifdef RX_THREADED_DISPATCH
    /* Indexed by opcode - the order is the RxOp enumeration's */
    static const void *const dispatch[] = {
        &&op_CHAR, &&op_CHAR_FOLD, &&op_ANY, &&op_ANY_ALL, &&op_CLASS, &&op_ALT, &&op_JMP,
        &&op_GROUP_BEGIN, &&op_GROUP_END, &&op_BOL, &&op_BOL_ML, &&op_EOL, &&op_EOL_ML, &&op_WB,
        &&op_NWB, &&op_BACKREF, &&op_LOOK_ATOM, &&op_LOOK, &&op_REPEAT_SIMPLE, &&op_REPEAT_ENTER,
        &&op_REPEAT_LOOP, &&op_REPEAT_NEXT, &&op_MATCH, &&op_ATOM_BACK, &&op_REPEAT_SIMPLE_BACK,
        &&op_GROUP_END_BACK, &&op_GROUP_BEGIN_BACK
    };
#define RX_CASE(name) op_##name:
#define RX_NEXT() \
    do { \
        inst = &prog[pc]; \
        goto *dispatch[inst->op]; \
    } while (0)
#else
#define RX_CASE(name) case RXI_##name:
#define RX_NEXT() continue
#endif

    const RxInst *inst;
    for (;;) {
        inst = &prog[pc];
#ifdef RX_THREADED_DISPATCH
        goto *dispatch[inst->op];
#else
        switch (inst->op) {
#endif

        RX_CASE(CHAR)
            if (pos >= length || rxCode(state, pos) != inst->operand) {
                goto backtrack;
            }
            pos++;
            pc++;
            RX_NEXT();

        RX_CASE(CHAR_FOLD)
            if (pos >= length || rxCanon(rxCode(state, pos)) != inst->operand) {
                goto backtrack;
            }
            pos++;
            pc++;
            RX_NEXT();

        RX_CASE(ANY)
            if (pos >= length || rxIsLineTerminator(rxCode(state, pos))) {
                goto backtrack;
            }
            pos++;
            pc++;
            RX_NEXT();

        RX_CASE(ANY_ALL)
            if (pos >= length) {
                goto backtrack;
            }
            pos++;
            pc++;
            RX_NEXT();

        RX_CASE(CLASS)
            if (pos >= length || !rxClassMatch(&state->classes[inst->operand], rxCode(state, pos))) {
                goto backtrack;
            }
            pos++;
            pc++;
            RX_NEXT();

        RX_CASE(JMP)
            pc = inst->a;
            RX_NEXT();

        RX_CASE(ALT) {
            const RxAlt *alt = &state->alts[inst->operand];
            size_t count = alt->count;
            if (++state->steps > RX_STEPS_MAX) {
                goto backtrack;
            }
            bool atEnd = pos >= length;
            uint32_t code = atEnd ? 0 : rxCode(state, pos);
            if (count <= RX_ALT_INDEX_MAX) {
                /* The alternatives that can begin here, as a mask; the rest wait on the stack */
                uint64_t mask;
                const RxAltIndex *index = alt->index;
                if (index != NULL) {
                    mask = index->always | (atEnd ? 0 : (code < 128 ? index->codes[code] : index->high));
                } else {
                    mask = 0;
                    for (size_t ix = 0; ix < count; ix++) {
                        if (rxAltViable(alt, ix, atEnd, code)) {
                            mask |= (uint64_t) 1 << ix;
                        }
                    }
                }
                if (mask == 0) {
                    goto backtrack;
                }
                unsigned ix = rxLowestBit64(mask);
                mask &= mask - 1;
                if (mask != 0) {
                    rxBtPush(state, RX_BT_ALT_MASK, pc, pos, mask);
                }
                pc = alt->branchPcs[ix];
            } else {
                size_t ix = 0;
                while (ix < count && !rxAltViable(alt, ix, atEnd, code)) {
                    ix++;
                }
                if (ix == count) {
                    goto backtrack;
                }
                if (ix + 1 < count) {
                    rxBtPush(state, RX_BT_ALT_INDEX, pc, pos, (uint32_t) (ix + 1));
                }
                pc = alt->branchPcs[ix];
            }
        }
        RX_NEXT();

        RX_CASE(GROUP_BEGIN)
            rxTrailPush(state, inst->a);
            state->match->groups[inst->a].begin = pos;
            pc++;
            RX_NEXT();

        RX_CASE(GROUP_END)
            rxTrailPush(state, inst->a);
            state->match->groups[inst->a].end = pos;
            state->match->matched[inst->a] = true;
            pc++;
            RX_NEXT();

        RX_CASE(GROUP_END_BACK)
            rxTrailPush(state, inst->a);
            state->match->groups[inst->a].end = pos;
            pc++;
            RX_NEXT();

        RX_CASE(GROUP_BEGIN_BACK)
            rxTrailPush(state, inst->a);
            state->match->groups[inst->a].begin = pos;
            state->match->matched[inst->a] = true;
            pc++;
            RX_NEXT();

        RX_CASE(ATOM_BACK)
            if (pos == 0 || !rxAtomAt(state, inst->aux & RX_ATOM_KIND, inst->operand, pos - 1)) {
                goto backtrack;
            }
            pos--;
            pc++;
            RX_NEXT();

        RX_CASE(BOL)
            if (pos != 0) {
                goto backtrack;
            }
            pc++;
            RX_NEXT();

        RX_CASE(BOL_ML)
            if (pos != 0 && !rxIsLineTerminator(rxCode(state, pos - 1))) {
                goto backtrack;
            }
            pc++;
            RX_NEXT();

        RX_CASE(EOL)
            if (pos != length) {
                goto backtrack;
            }
            pc++;
            RX_NEXT();

        RX_CASE(EOL_ML)
            if (pos != length && !rxIsLineTerminator(rxCode(state, pos))) {
                goto backtrack;
            }
            pc++;
            RX_NEXT();

        RX_CASE(WB)
        RX_CASE(NWB) {
            bool before = pos > 0 && rxIsWordCode(rxCode(state, pos - 1));
            bool after = pos < length && rxIsWordCode(rxCode(state, pos));
            if ((before != after) != (inst->op == RXI_WB)) {
                goto backtrack;
            }
            pc++;
        }
        RX_NEXT();

        RX_CASE(BACKREF) {
            size_t group = inst->a;
            if (inst->b != 0) {
                /* Of the groups sharing the name, the one that took part - at most one can have */
                const uint32_t *candidates = state->backrefGroups + inst->a;
                group = candidates[0];
                for (uint32_t ix = 1; ix < inst->b; ix++) {
                    if (state->match->matched[candidates[ix]]) {
                        group = candidates[ix];
                    }
                }
            }
            if (group < state->match->groupCount && state->match->matched[group]) {
                /* The captured text follows - or, matching backward, precedes - the position */
                BSRegexSpan span = state->match->groups[group];
                size_t size = span.end - span.begin;
                bool back = (inst->aux & RX_BACKREF_BACK) != 0;
                if (back ? pos < size : pos + size > length) {
                    goto backtrack;
                }
                size_t at = back ? pos - size : pos;
                for (size_t ix = 0; ix < size; ix++) {
                    uint32_t expected = rxCode(state, span.begin + ix);
                    uint32_t actual = rxCode(state, at + ix);
                    if ((inst->aux & RX_BACKREF_FOLD) != 0 ? rxCanon(expected) != rxCanon(actual) : expected != actual) {
                        goto backtrack;
                    }
                }
                pos = back ? at : pos + size;
            }
            /* An unmatched backreference matches the empty string */
            pc++;
        }
        RX_NEXT();

        RX_CASE(LOOK_ATOM) {
            unsigned kind = inst->aux & RX_ATOM_KIND;
            bool matched = (inst->aux & RX_LOOK_BEHIND) == 0 ? rxAtomAt(state, kind, inst->operand, pos) :
                (pos >= 1 && rxAtomAt(state, kind, inst->operand, pos - 1));
            if (matched == ((inst->aux & RX_ATOM_FLAG) != 0)) {
                goto backtrack;
            }
            pc++;
        }
        RX_NEXT();

        RX_CASE(LOOK) {
            /* The body runs from here - forward, or backward for a lookbehind, as it was emitted */
            size_t mark = state->trailCount;
            bool matched = rxRun(state, inst->a, pos);
            bool negate = inst->aux != 0;
            if (negate || !matched) {
                rxTrailUnwind(state, mark);
            }
            if (matched == negate) {
                goto backtrack;
            }
            pc = inst->b;
        }
        RX_NEXT();

        RX_CASE(REPEAT_SIMPLE) {
            unsigned kind = inst->aux & RX_ATOM_KIND;
            uint32_t operand = inst->operand;
            size_t min = inst->a;
            uint32_t max = inst->b;
            uint32_t next = pc + 1;
            size_t end = pos;

            if ((inst->aux & RX_ATOM_FLAG) == 0) {
                /* Lazy: take the minimum, then one more each time the continuation fails - a count
                   saturates below the unbounded marker, so a bound means "min < max" */
                while (end - pos < min && rxAtomAt(state, kind, operand, end)) {
                    end++;
                }
                if (end - pos < min) {
                    goto backtrack;
                }
                if (min < max) {
                    rxBtPush(state, RX_BT_LAZY, next, end, (uint32_t) pos);
                }
                pos = end;
                pc = next;
                goto dispatch;
            }
            size_t limit = (max == RX_UNBOUNDED || max > length - pos) ? length : pos + max;

            /* Consume as much as the body matches - an ASCII subject scans by the body's kind */
            if (state->codes == NULL) {
                const unsigned char *bytes = state->bytes;
                switch (kind) {
                case RXI_CHAR:
                    while (end < limit && bytes[end] == operand) {
                        end++;
                    }
                    break;
                case RXI_CHAR_FOLD:
                    while (end < limit && rxCanon(bytes[end]) == operand) {
                        end++;
                    }
                    break;
                case RXI_ANY:
                    while (end < limit && bytes[end] != '\n' && bytes[end] != '\r') {
                        end++;
                    }
                    break;
                case RXI_ANY_ALL:
                    end = limit;
                    break;
                default: {
                    const uint8_t *table = state->classes[operand].ascii;
                    while (end < limit && ((table[bytes[end] >> 3] >> (bytes[end] & 7)) & 1u) != 0) {
                        end++;
                    }
                    break;
                }
                }
            } else {
                while (end < limit && rxAtomAt(state, kind, operand, end)) {
                    end++;
                }
            }
            if (end - pos < min) {
                goto backtrack;
            }
            size_t stop = pos + min;

            /* Give back one at a time. When a literal must follow, only positions holding it can go on. */
            if (prog[next].op == RXI_CHAR && !rxGiveBackTo(state, prog[next].operand, stop, &end)) {
                goto backtrack;
            }
            if (end > stop) {
                rxBtPush(state, RX_BT_GIVEBACK, next, end, (uint32_t) stop);
            }
            pos = end;
            pc = next;
        }
        RX_NEXT();

        RX_CASE(REPEAT_ENTER)
            rxTrailPushRepeat(state, inst->slot);
            state->repeats[inst->slot].count = 0;
            pc++;
            RX_NEXT();

        RX_CASE(REPEAT_LOOP) {
            uint32_t slot = inst->slot;
            size_t count = state->repeats[slot].count;
            if (++state->steps > RX_STEPS_MAX) {
                goto backtrack;
            }
            if (inst->b != RX_UNBOUNDED && count >= inst->b) {
                pc = inst->a;
                goto dispatch;
            }
            if (count >= inst->operand) {
                if (inst->aux == 0) {
                    /* Lazy: try the continuation first; the body is the alternative */
                    rxBtPush(state, RX_BT_REPEAT_BODY, pc, pos, 0);
                    pc = inst->a;
                    goto dispatch;
                }
                rxBtPush(state, RX_BT_SPLIT, inst->a, pos, 0);
            }
            rxRepeatEnter(state, slot, pos);
            pc++;
        }
        RX_NEXT();

        RX_CASE(REPEAT_NEXT) {
            /*
             * An iteration that consumed nothing ends the repetition - and, past the required
             * iterations, fails, so the repetition stands as it was before it, as JavaScript's does
             */
            uint32_t slot = inst->slot;
            if (pos != state->repeats[slot].start) {
                pc = inst->a;
            } else if (state->repeats[slot].count > inst->operand) {
                goto backtrack;
            } else {
                pc = inst->b;
            }
        }
        RX_NEXT();

        RX_CASE(MATCH)
            state->end = pos;
            state->btCount = btBase;
            return true;

        RX_CASE(REPEAT_SIMPLE_BACK) {
            /* The simple repeat matching backward: its atoms end at pos and are taken toward the start */
            unsigned kind = inst->aux & RX_ATOM_KIND;
            uint32_t operand = inst->operand;
            size_t min = inst->a;
            uint32_t max = inst->b;
            uint32_t next = pc + 1;
            size_t end = pos;
            if ((inst->aux & RX_ATOM_FLAG) == 0) {
                while (pos - end < min && end > 0 && rxAtomAt(state, kind, operand, end - 1)) {
                    end--;
                }
                if (pos - end < min) {
                    goto backtrack;
                }
                if (min < max) {
                    rxBtPush(state, RX_BT_LAZY_BACK, next, end, (uint32_t) pos);
                }
                pos = end;
                pc = next;
                goto dispatch;
            }
            size_t limit = (max == RX_UNBOUNDED || max > pos) ? 0 : pos - max;
            while (end > limit && rxAtomAt(state, kind, operand, end - 1)) {
                end--;
            }
            if (pos - end < min) {
                goto backtrack;
            }
            size_t stop = pos - min;
            if (end < stop) {
                rxBtPush(state, RX_BT_GIVEBACK_BACK, next, end, (uint32_t) stop);
            }
            pos = end;
            pc = next;
        }
        RX_NEXT();

#ifndef RX_THREADED_DISPATCH
        }
#endif
    dispatch:
        RX_NEXT();

    backtrack:
        for (;;) {
            /* A failed run leaves no capture or counter behind - the next attempt would read a stale span */
            if (state->btCount == btBase) {
                rxTrailUnwind(state, trailBase);
                return false;
            }
            if (++state->steps > RX_STEPS_MAX) {
                state->btCount = btBase;
                rxTrailUnwind(state, trailBase);
                return false;
            }
            RxBacktrack *bt = &state->bt[state->btCount - 1];
            /* Every attempt from an entry starts at its trail mark - and so does the next entry's, once
               this one is popped, since a deeper entry's mark is never below a shallower one's */
            rxTrailUnwind(state, bt->trail);
            switch (bt->kind) {

            case RX_BT_SPLIT:
                pc = bt->pc;
                pos = bt->pos;
                state->btCount--;
                break;

            case RX_BT_REPEAT_BODY:
                pc = bt->pc + 1;
                pos = bt->pos;
                state->btCount--;
                rxRepeatEnter(state, prog[bt->pc].slot, pos);
                break;

            case RX_BT_ALT_MASK: {
                uint64_t mask = bt->aux;
                unsigned ix = rxLowestBit64(mask);
                mask &= mask - 1;
                pc = state->alts[prog[bt->pc].operand].branchPcs[ix];
                pos = bt->pos;
                if (mask != 0) {
                    bt->aux = mask;
                    break;
                }
                state->btCount--;
                break;
            }

            case RX_BT_ALT_INDEX: {
                const RxAlt *alt = &state->alts[prog[bt->pc].operand];
                size_t count = alt->count;
                bool atEnd = bt->pos >= length;
                uint32_t code = atEnd ? 0 : rxCode(state, bt->pos);
                size_t ix = bt->aux;
                while (ix < count && !rxAltViable(alt, ix, atEnd, code)) {
                    ix++;
                }
                if (ix == count) {
                    state->btCount--;
                    continue;
                }
                pc = alt->branchPcs[ix];
                pos = bt->pos;
                if (ix + 1 < count) {
                    bt->aux = (uint32_t) (ix + 1);
                    break;
                }
                state->btCount--;
                break;
            }

            case RX_BT_GIVEBACK: {
                /* The entry is only kept while there is more to give back, so end is past stop */
                size_t end = bt->pos - 1;
                size_t stop = bt->aux;
                if (prog[bt->pc].op == RXI_CHAR && !rxGiveBackTo(state, prog[bt->pc].operand, stop, &end)) {
                    state->btCount--;
                    continue;
                }
                pc = bt->pc;
                pos = end;
                if (end > stop) {
                    bt->pos = (uint32_t) end;
                    break;
                }
                state->btCount--;
                break;
            }

            case RX_BT_LAZY: {
                /* One more of the body, while the bound and the body allow */
                const RxInst *repeat = &prog[bt->pc - 1];
                size_t end = bt->pos;
                size_t count = end - bt->aux;
                if ((repeat->b != RX_UNBOUNDED && count >= repeat->b) ||
                    !rxAtomAt(state, repeat->aux & RX_ATOM_KIND, repeat->operand, end)) {
                    state->btCount--;
                    continue;
                }
                end++;
                pc = bt->pc;
                pos = end;
                bt->pos = (uint32_t) end;
                break;
            }

            case RX_BT_GIVEBACK_BACK: {
                /* Give back one to the right, while there is more than the minimum */
                size_t end = bt->pos + 1;
                pc = bt->pc;
                pos = end;
                if (end < bt->aux) {
                    bt->pos = (uint32_t) end;
                    break;
                }
                state->btCount--;
                break;
            }

            default: {
                /* RX_BT_LAZY_BACK - one more of the body to the left, while the bound and the body allow */
                const RxInst *repeat = &prog[bt->pc - 1];
                size_t end = bt->pos;
                size_t count = bt->aux - end;
                if ((repeat->b != RX_UNBOUNDED && count >= repeat->b) || end == 0 ||
                    !rxAtomAt(state, repeat->aux & RX_ATOM_KIND, repeat->operand, end - 1)) {
                    state->btCount--;
                    continue;
                }
                end--;
                pc = bt->pc;
                pos = end;
                bt->pos = (uint32_t) end;
                break;
            }
            }
            break;
        }
    }
    return false; /* GCOV_EXCL_LINE - the loop leaves only by returning; this satisfies the compiler */
}
#undef RX_CASE
#undef RX_NEXT

void bsRegexSubjectInit(BSRegexSubject *subject, BSValue string)
{
    const char *data = bsStringData(string);
    size_t size = bsStringSize(string);
    size_t length = bsStringLength(string);
    subject->length = length;
    subject->owned = NULL;

    /* ASCII: match the original bytes; code point i is bytes[i] */
    if (length == size) {
        subject->codes = NULL;
        subject->bytes = (const unsigned char *) data;
        return;
    }

    subject->bytes = NULL;
    uint32_t *codes = subject->inline_;
    if (length > sizeof(subject->inline_) / sizeof(subject->inline_[0])) {
        subject->owned = bsAlloc(length * sizeof(uint32_t));
        codes = subject->owned;
    }
    subject->codes = codes;
    size_t offset = 0;
    size_t index = 0;
    while (offset < size && index < length) {
        size_t codeSize;
        codes[index++] = bsUTF8Decode(data, size, offset, &codeSize);
        offset += codeSize;
    }
}


void bsRegexSubjectFree(BSRegexSubject *subject)
{
    free(subject->owned);
}


bool bsRegexSearch(BSValue regex, const BSRegexSubject *subject, size_t start, BSRegexMatch *match)
{
    BSRegex *compiled = regex.u.regex;
    RxScratch *scratch = &bsRxScratch;
    if (scratch->trail == NULL) {
        scratch->trailCapacity = RX_TRAIL_INITIAL;
        scratch->trail = bsAlloc(scratch->trailCapacity * sizeof(RxTrailEntry));
        scratch->btCapacity = RX_BACKTRACK_INITIAL;
        scratch->bt = bsAlloc(scratch->btCapacity * sizeof(RxBacktrack));
    }
    if (compiled->repeatCount > scratch->repeatCapacity) {
        scratch->repeatCapacity = compiled->repeatCount;
        scratch->repeats = bsRealloc(scratch->repeats, scratch->repeatCapacity * sizeof(RxRepeat));
    }
    RxState state;
    state.codes = subject->codes;
    state.bytes = subject->bytes;
    state.length = subject->length;
    state.match = match;
    state.trail = scratch->trail;
    state.trailCapacity = scratch->trailCapacity;
    state.prog = compiled->prog;
    state.classes = compiled->classes;
    state.alts = compiled->alts;
    state.bt = scratch->bt;
    state.btCapacity = scratch->btCapacity;
    state.repeats = scratch->repeats;
    state.repeatGroups = compiled->repeatGroups;
    state.backrefGroups = compiled->backrefGroups;

    /*
     * Only the matched flags need clearing - a span is read only once its flag is set - and only
     * once: every capture write is on the trail, and a failed attempt unwinds all of them. All of
     * them are cleared, a fixed size the compiler stores in place rather than a call.
     */
    memset(match->matched, 0, sizeof(match->matched));
    match->groupCount = compiled->groupCount;
    size_t last = compiled->anchored ? start : subject->length;
    bool found = false;
    for (size_t pos = start; pos <= last && !found; pos++) {
        /* Skip positions whose code point cannot begin a match */
        if (!compiled->first.any) {
            size_t length = subject->length;
            if (subject->codes != NULL) {
                const uint32_t *codes = subject->codes;
                while (pos < length && !rxFirstHas(&compiled->first, codes[pos])) {
                    pos++;
                }
            } else if (compiled->firstByte >= 0) {
                const unsigned char *at = pos < length ?
                    memchr(subject->bytes + pos, compiled->firstByte, length - pos) : NULL;
                pos = at != NULL ? (size_t) (at - subject->bytes) : length;
            } else {
                const unsigned char *bytes = subject->bytes;
                const uint8_t *firstBytes = compiled->firstBytes;
                while (pos < length && !firstBytes[bytes[pos]]) {
                    pos++;
                }
            }
            if (pos >= length) {
                break;
            }
        }
        state.steps = 0;
        state.trailCount = 0;
        state.btCount = 0;
        found = rxRun(&state, 0, pos);
        if (found) {
            match->begin = pos;
            match->end = state.end;
            match->groups[0].begin = pos;
            match->groups[0].end = state.end;
            match->matched[0] = true;
        }
    }
    /* Keep what the search grew */
    scratch->trail = state.trail;
    scratch->trailCapacity = state.trailCapacity;
    scratch->bt = state.bt;
    scratch->btCapacity = state.btCapacity;
    return found;
}


BSValue bsRegexEscape(BSValue string)
{
    static const char *special = ".*+?^${}()|[]\\";
    const char *data = bsStringData(string);
    size_t size = bsStringSize(string);
    BSStringBuilder sb;
    bsSBInit(&sb);
    for (size_t ix = 0; ix < size; ix++) {
        char ch = data[ix];
        if (ch != '\0' && strchr(special, ch) != NULL) {
            bsSBAppendChar(&sb, '\\');
        }
        bsSBAppendChar(&sb, ch);
    }
    return bsSBToValue(&sb);
}
