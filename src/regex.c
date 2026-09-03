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
        size_t groupIndex; /* RX_BACKREF */
        struct {
            RxNode *sub;
            int min;
            int max; /* -1 for unbounded */
            bool greedy;
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
    bool codes[256];
    bool high;   /* a match can begin with a code point of 256 or more */
    bool any;    /* a match can begin with anything, so the set is not usable */
} RxFirstSet;


/*
 * An alternative's first-code-point set, packed
 *
 * The markdown span alternation has sixteen alternatives, and a candidate position - one the
 * pattern's first set admits - used to try every one of them. Each alternative's own first set
 * lets the alternation skip the ones that cannot begin with the code point at hand.
 */
typedef struct RxBranchFirst {
    uint64_t bits[4]; /* membership of the code points 0 - 255 */
    bool high;
    bool usable;      /* false if the alternative can match the empty string or begin with anything */
} RxBranchFirst;


static inline bool rxBranchFirstHas(const RxBranchFirst *first, uint32_t code)
{
    return code >= 256 ? first->high : (first->bits[code >> 6] >> (code & 63)) & 1u;
}


/*
 * A wide alternation's alternatives indexed by first code point: "codes" holds, per code point,
 * the set of alternatives that can begin with it as a bit per alternative, so the alternation
 * tries just those, in order, instead of testing every alternative's set. "always" holds the
 * alternatives whose sets are not usable. Built for alternations of eight to thirty-two
 * alternatives - the markdown span alternation has sixteen.
 */
#define RX_ALT_INDEX_MIN 8
#define RX_ALT_INDEX_MAX 32

typedef struct RxAltIndex {
    uint32_t codes[256];
    uint32_t high;   /* the alternatives that can begin with a code point of 256 or more */
    uint32_t always;
} RxAltIndex;


static inline unsigned rxLowestBit(uint32_t mask)
{
#if defined(__GNUC__)
    return (unsigned) __builtin_ctz(mask);
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
static void rxEmitProgram(struct BSRegex *regex);
static void rxProgramFree(struct BSRegex *regex);

struct BSRegex {
    int32_t refcount;
    unsigned flags;
    bool anchored; /* every alternative begins with "^", so only the start position can match */
    bool firstUsable;
    RxFirstSet first;
    RxNode *root;
    size_t groupCount;
    BSValue *groupNames;  /* groupCount entries, or NULL if no group is named */
    bool uniqueNames;     /* no two groups share a name, so a match model can append each */
    RxNodeChunk *chunks;
    RxInst *prog;         /* the compiled program, and the tables its instructions refer to */
    size_t progCount;
    struct RxClass *classes;
    size_t classCount;
    struct RxAlt *alts;
    size_t altCount;
    struct RxLook *looks;
    size_t lookCount;
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


/* The single-code-point node a sub-pattern reduces to, or NULL */
static RxNode *rxSimpleAtom(RxNode *node)
{
    if (node != NULL && node->kind == RX_ALT && node->u.alt.count == 1 && node->next == NULL) {
        node = node->u.alt.branches[0];
    }
    return (node != NULL && rxIsSimple(node)) ? node : NULL;
}


typedef struct RxCompiler {
    const char *pattern;
    size_t size;
    size_t offset;
    unsigned flags;
    BSRegex *regex;
    char *error;      /* the caller's message buffer, empty until a failure */
    size_t errorSize;
    bool failed;
    BSValue groupNames[BS_REGEX_GROUPS_MAX];
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
 * The position a compilation error reports
 *
 * The Python implementation compiles a translated pattern - "(?<name>" becomes "(?P<name>" and
 * "\k<name>" becomes "(?P=name)", each exactly one character longer - so the position it reports
 * is this pattern's position plus the number of translations that precede it.
 */
static size_t rxErrorPosition(const RxCompiler *compiler, size_t position)
{
    size_t translated = position;
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


static uint32_t rxFold(uint32_t ch)
{
    return (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
}


static uint32_t rxSwapCase(uint32_t ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return ch + 32;
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 32;
    }
    return ch;
}


static RxNode *rxNodeNew(RxCompiler *compiler, RxKind kind)
{
    BSRegex *regex = compiler->regex;
    RxNodeChunk *chunk = regex->chunks;
    if (chunk == NULL || chunk->used == RX_CHUNK_NODES) {
        chunk = bsAlloc(sizeof(RxNodeChunk));
        chunk->next = regex->chunks;
        chunk->used = 0;
        regex->chunks = chunk;
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
    node->u.ch = (compiler->flags & BS_REGEX_IGNORECASE) != 0 ? rxFold(ch) : ch;
    return node;
}


/*
 * Parse a fixed-length hexadecimal escape
 *
 * On failure the escape is reported as far as it reads, which is what Python's "incomplete escape"
 * message shows.
 */
static bool rxHex(RxCompiler *compiler, size_t count, char kind, size_t escapeOffset, uint32_t *result)
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
        return false;
    }
    compiler->offset += count;
    *result = value;
    return true;
}


/*
 * Append a digit to a "{n,m}" count, saturating at INT_MAX. JavaScript accepts any count, and a
 * count that large never matches anyway; CPython raises an OverflowError the Python
 * implementation does not catch.
 */
static int rxRepeatDigit(int count, char digit)
{
    return count > (INT_MAX - 9) / 10 ? INT_MAX : count * 10 + (digit - '0');
}


/* Accumulate the decimal digits at the offset into "*count", saturating; returns how many there were */
static size_t rxDigits(RxCompiler *compiler, int *count)
{
    size_t digits = 0;
    *count = 0;
    while (compiler->offset < compiler->size && compiler->pattern[compiler->offset] >= '0' &&
           compiler->pattern[compiler->offset] <= '9') {
        *count = rxRepeatDigit(*count, compiler->pattern[compiler->offset]);
        compiler->offset++;
        digits++;
    }
    return digits;
}


/* Step past the backslash at "escapeOffset": an escape needs a character after it */
static bool rxEscapeBegin(RxCompiler *compiler, size_t escapeOffset)
{
    compiler->offset = escapeOffset + 1;
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
        *literal = 0;
        return 0;
    case 'x':
        rxHex(compiler, 2, 'x', escapeOffset, literal);
        return 0;
    case 'u':
        rxHex(compiler, 4, 'u', escapeOffset, literal);
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
        if (!rxEscapeBegin(compiler, compiler->offset)) {
            return false;
        }
        if (compiler->pattern[compiler->offset] == 'b') {
            compiler->offset++;
            *code = '\b';
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
        uint32_t lo;
        unsigned classes;
        if (!rxClassBound(compiler, &lo, &classes)) {
            return NULL;
        }
        if (classes != 0) {
            node->u.cls.classes |= classes;
            continue;
        }

        /* The optional range's high bound */
        size_t lowEnd = compiler->offset;
        uint32_t hi = lo;
        if (compiler->offset + 1 < compiler->size && compiler->pattern[compiler->offset] == '-' &&
            compiler->pattern[compiler->offset + 1] != ']') {
            compiler->offset++;
            size_t highOffset = compiler->offset;
            if (!rxClassBound(compiler, &hi, &classes)) {
                return NULL;
            }
            if (classes != 0 || hi < lo) {
                rxError(compiler, lowOffset, "bad character range %.*s-%.*s",
                        (int) (lowEnd - lowOffset), compiler->pattern + lowOffset,
                        (int) (compiler->offset - highOffset), compiler->pattern + highOffset);
                return NULL;
            }
        }
        rxClassRange(node, lo, hi);
    }

    rxError(compiler, classOffset, "unterminated character set");
    return NULL;
}


static bool rxParseName(RxCompiler *compiler, BSValue *name)
{
    size_t begin = compiler->offset;
    while (compiler->offset < compiler->size && compiler->pattern[compiler->offset] != '>') {
        char ch = compiler->pattern[compiler->offset];
        if (!rxIsWordCode((unsigned char) ch)) {
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


static RxNode *rxParseAtom(RxCompiler *compiler)
{
    char ch = compiler->pattern[compiler->offset];

    /* Group, non-capturing group, named group, or lookahead */
    if (ch == '(') {
        size_t groupOffset = compiler->offset;
        compiler->offset++;
        bool capture = true;
        BSValue name = bsNull();
        bool lookahead = false;
        bool lookbehind = false;
        bool lookaheadNegate = false;
        if (compiler->offset < compiler->size && compiler->pattern[compiler->offset] == '?') {
            size_t extensionOffset = compiler->offset;
            compiler->offset++;
            if (compiler->offset >= compiler->size) {
                rxError(compiler, compiler->offset, "unexpected end of pattern");
                return NULL;
            }
            size_t kindOffset = compiler->offset;
            char kind = compiler->pattern[compiler->offset++];
            if (kind == ':') {
                capture = false;
            } else if (kind == '=' || kind == '!') {
                capture = false;
                lookahead = true;
                lookaheadNegate = (kind == '!');
            } else if (kind == '<' && compiler->offset < compiler->size &&
                       (compiler->pattern[compiler->offset] == '=' ||
                        compiler->pattern[compiler->offset] == '!')) {
                capture = false;
                lookbehind = true;
                lookaheadNegate = (compiler->pattern[compiler->offset] == '!');
                compiler->offset++;
            } else if (kind == '<') {
                if (compiler->offset >= compiler->size) {
                    rxError(compiler, compiler->offset, "unexpected end of pattern");
                    return NULL;
                }
                size_t nameOffset = compiler->offset;
                if (!rxParseName(compiler, &name)) {
                    rxError(compiler, extensionOffset, "unknown extension ?<%.*s",
                            (int) rxTokenSize(compiler, nameOffset),
                            compiler->pattern + nameOffset);
                    return NULL;
                }
            } else {
                rxError(compiler, extensionOffset, "unknown extension ?%.*s",
                        (int) rxTokenSize(compiler, kindOffset), compiler->pattern + kindOffset);
                return NULL;
            }
        }

        size_t group = 0;
        if (capture) {
            if (compiler->regex->groupCount >= BS_REGEX_GROUPS_MAX) {
                bsRelease(name);
                rxError(compiler, groupOffset, "sorry, but this version only supports %d groups",
                        BS_REGEX_GROUPS_MAX - 1);
                return NULL;
            }
            group = compiler->regex->groupCount++;
            compiler->groupNames[group] = name;
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

        if (lookahead || lookbehind) {
            RxNode *node = rxNodeNew(compiler, lookbehind ? RX_LOOKBEHIND : RX_LOOKAHEAD);
            node->u.look.sub = sub;
            node->u.look.negate = lookaheadNegate;
            return node;
        }
        if (!capture) {
            /* A non-capturing group is its alternation node - which always has a free "next" */
            return sub;
        }
        RxNode *node = rxNodeNew(compiler, RX_GROUP);
        node->u.group.sub = sub;
        node->u.group.group = group;
        return node;
    }

    if (ch == '[') {
        size_t classOffset = compiler->offset;
        compiler->offset++;
        return rxParseClass(compiler, classOffset);
    }

    if (ch == '.') {
        compiler->offset++;
        return rxNodeNew(compiler, RX_ANY);
    }

    if (ch == '^') {
        compiler->offset++;
        return rxNodeNew(compiler, RX_BOL);
    }

    if (ch == '$') {
        compiler->offset++;
        return rxNodeNew(compiler, RX_EOL);
    }

    if (ch == '\\') {
        size_t escapeOffset = compiler->offset;
        if (!rxEscapeBegin(compiler, escapeOffset)) {
            return NULL;
        }
        char escape = compiler->pattern[compiler->offset];

        /* Word boundaries */
        if (escape == 'b' || escape == 'B') {
            compiler->offset++;
            return rxNodeNew(compiler, escape == 'b' ? RX_WORD_BOUNDARY : RX_NOT_WORD_BOUNDARY);
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
            RxNode *node = rxNodeNew(compiler, RX_BACKREF);
            node->u.groupIndex = (size_t) group;
            return node;
        }

        /* A named backreference */
        if (escape == 'k') {
            if (compiler->offset + 1 >= compiler->size ||
                compiler->pattern[compiler->offset + 1] != '<') {
                rxError(compiler, escapeOffset, "bad escape \\k");
                return NULL;
            }
            compiler->offset += 2;
            size_t nameOffset = compiler->offset;
            BSValue name;
            if (!rxParseName(compiler, &name)) {
                rxError(compiler, escapeOffset, "bad escape \\k");
                return NULL;
            }
            size_t group = 0;
            for (size_t ix = 1; ix < compiler->regex->groupCount; ix++) {
                if (compiler->groupNames[ix].type == BS_STRING &&
                    bsValueCompare(compiler->groupNames[ix], name) == 0) {
                    group = ix;
                    break;
                }
            }
            if (group == 0) {
                rxError(compiler, nameOffset, "unknown group name '%s'", bsStringData(name));
                bsRelease(name);
                return NULL;
            }
            bsRelease(name);
            RxNode *node = rxNodeNew(compiler, RX_BACKREF);
            node->u.groupIndex = group;
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

    while (compiler->offset < compiler->size) {
        char ch = compiler->pattern[compiler->offset];
        if (ch == '|' || ch == ')') {
            break;
        }

        /* A quantifier with no atom to repeat */
        size_t quantifierOffset = compiler->offset;
        int min;
        int max;
        size_t digitOffset;
        if (rxMatchQuantifier(compiler, &min, &max, &digitOffset)) {
            rxError(compiler, quantifierOffset, "nothing to repeat");
            return NULL;
        }

        RxNode *atom = rxParseAtom(compiler);
        if (atom == NULL) {
            return NULL;
        }

        /* An optional quantifier - an assertion has nothing to repeat */
        size_t repeatOffset = compiler->offset;
        if (rxMatchQuantifier(compiler, &min, &max, &digitOffset)) {
            if (atom->kind == RX_BOL || atom->kind == RX_EOL ||
                atom->kind == RX_WORD_BOUNDARY || atom->kind == RX_NOT_WORD_BOUNDARY) {
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
            atom = repeat;

            /* A quantifier cannot itself be quantified */
            size_t secondOffset = compiler->offset;
            if (rxMatchQuantifier(compiler, &min, &max, &digitOffset)) {
                rxError(compiler, secondOffset, "multiple repeat");
                return NULL;
            }
        }

        *tail = atom;
        tail = &atom->next;
    }

    return head;
}

static RxNode *rxParseAlternation(RxCompiler *compiler)
{
    RxNode **branches = NULL;
    size_t count = 0;
    size_t capacity = 0;

    while (true) {
        RxNode *branch = rxParseSequence(compiler);
        if (compiler->failed) {
            free(branches);
            return NULL;
        }
        if (count == capacity) {
            capacity = capacity != 0 ? capacity * 2 : 8;
            branches = bsRealloc(branches, capacity * sizeof(RxNode *));
        }
        branches[count++] = branch;
        if (compiler->offset >= compiler->size || compiler->pattern[compiler->offset] != '|') {
            break;
        }
        compiler->offset++;
    }

    RxNode *alt = rxNodeNew(compiler, RX_ALT);
    alt->u.alt.branches = branches;
    alt->u.alt.count = count;
    return alt;
}


/* The unbounded match length */
#define RX_LENGTH_MAX SIZE_MAX


static size_t rxLengthAdd(size_t left, size_t right)
{
    if (left == RX_LENGTH_MAX || right == RX_LENGTH_MAX) {
        return RX_LENGTH_MAX;
    }
    return left + right;
}


static size_t rxLengthMul(size_t length, int count)
{
    return length == RX_LENGTH_MAX ? RX_LENGTH_MAX : length * (size_t) count;
}


/* Compute a node chain's minimum and maximum match length, in code points */
static void rxNodeLength(const RxNode *node, size_t *minLength, size_t *maxLength)
{
    *minLength = 0;
    *maxLength = 0;
    for (; node != NULL; node = node->next) {
        size_t nodeMin = 0;
        size_t nodeMax = 0;
        switch (node->kind) {
        case RX_CHAR:
        case RX_ANY:
        case RX_CLASS:
            nodeMin = 1;
            nodeMax = 1;
            break;

        case RX_ALT: {
            nodeMin = RX_LENGTH_MAX;
            nodeMax = 0;
            for (size_t ix = 0; ix < node->u.alt.count; ix++) {
                size_t branchMin;
                size_t branchMax;
                rxNodeLength(node->u.alt.branches[ix], &branchMin, &branchMax);
                nodeMin = branchMin < nodeMin ? branchMin : nodeMin;
                nodeMax = branchMax > nodeMax ? branchMax : nodeMax;
            }
            break;
        }

        case RX_GROUP:
            rxNodeLength(node->u.group.sub, &nodeMin, &nodeMax);
            break;

        case RX_REPEAT: {
            size_t subMin;
            size_t subMax;
            rxNodeLength(node->u.repeat.sub, &subMin, &subMax);
            nodeMin = rxLengthMul(subMin, node->u.repeat.min);
            nodeMax = node->u.repeat.max < 0 ? (subMax == 0 ? 0 : RX_LENGTH_MAX) :
                rxLengthMul(subMax, node->u.repeat.max);
            break;
        }

        case RX_BACKREF:
            nodeMin = 0;
            nodeMax = RX_LENGTH_MAX;
            break;

        default:
            /* Anchors and lookaround match the empty string */
            break;
        }
        *minLength = rxLengthAdd(*minLength, nodeMin);
        *maxLength = rxLengthAdd(*maxLength, nodeMax);
    }
}


/* Add a code point, and its other case when matching case-insensitively, to a first set */
static void rxFirstAddCode(RxFirstSet *set, unsigned flags, uint32_t code)
{
    if (code >= 256) {
        set->high = true;
        return;
    }
    set->codes[code] = true;
    if ((flags & BS_REGEX_IGNORECASE) != 0) {
        uint32_t other = rxSwapCase(code);
        if (other < 256) {
            set->codes[other] = true;
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

        case RX_CLASS:
            /* A negated or predefined class can match code points a table cannot enumerate */
            if (node->u.cls.negate || node->u.cls.classes != 0) {
                set->any = true;
                return false;
            }
            for (size_t ix = 0; ix < node->u.cls.rangeCount; ix++) {
                uint32_t low = node->u.cls.ranges[ix * 2];
                uint32_t high = node->u.cls.ranges[ix * 2 + 1];
                if (high >= 256) {
                    set->high = true;
                    high = 255;
                }
                for (uint32_t code = low; code <= high && code < 256; code++) {
                    rxFirstAddCode(set, flags, code);
                }
            }
            return false;

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


/* Free the node tree - once the program is compiled it is not needed, and on a failed compile */
static void rxChunksFree(BSRegex *regex)
{
    RxNodeChunk *chunk = regex->chunks;
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
    regex->chunks = NULL;
    regex->root = NULL;
}


static void bsRegexFree(BSRegex *regex)
{
    rxChunksFree(regex);
    rxProgramFree(regex);
    if (regex->groupNames != NULL) {
        for (size_t ix = 0; ix < regex->groupCount; ix++) {
            bsRelease(regex->groupNames[ix]);
        }
        free(regex->groupNames);
    }
    free(regex);
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
    regex->flags = flags;
    regex->groupCount = 1;

    RxCompiler compiler;
    memset(&compiler, 0, sizeof(compiler));
    compiler.pattern = pattern;
    compiler.size = patternSize;
    compiler.offset = 0;
    compiler.flags = flags;
    compiler.regex = regex;
    compiler.error = error;
    compiler.errorSize = errorSize;
    regex->root = rxParseAlternation(&compiler);
    if (!compiler.failed && compiler.offset != patternSize) {
        rxError(&compiler, compiler.offset, "unbalanced parenthesis");
    }
    if (!compiler.failed) {
        /*
         * A pattern whose every alternative begins with "^" can only match at the search start, so
         * the scan over later positions is skipped. Multi-line patterns still scan, since "^" also
         * matches after a newline. The parser's patterns are all anchored, so this is the
         * difference between a linear and a quadratic scan over every line it parses.
         */
        if ((flags & BS_REGEX_MULTILINE) == 0) {
            regex->anchored = true;
            for (size_t ix = 0; ix < regex->root->u.alt.count; ix++) {
                const RxNode *branch = regex->root->u.alt.branches[ix];
                if (branch == NULL || branch->kind != RX_BOL) {
                    regex->anchored = false;
                    break;
                }
            }
        }

        /* Compute the set of code points a match can begin with, for the search scan */
        if (!regex->anchored) {
            regex->firstUsable = !rxFirstSet(regex->root, flags, &regex->first) && !regex->first.any;
        }

        rxEmitProgram(regex);
        rxChunksFree(regex);

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
    }
    if (compiler.failed) {
        for (size_t ix = 0; ix < regex->groupCount; ix++) {
            bsRelease(compiler.groupNames[ix]);
        }
        bsRegexFree(regex);
        return bsNull();
    }

    BSValue value;
    value.type = BS_REGEX;
    value.u.regex = regex;
    return value;
}


void bsRegexRetain(BSValue value)
{
    value.u.regex->refcount++;
}


void bsRegexRelease(BSValue value)
{
    if (--value.u.regex->refcount == 0) {
        bsRegexFree(value.u.regex);
    }
}


bool bsRegexGroupNamesUnique(BSValue regex)
{
    return regex.u.regex->uniqueNames;
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
    RXI_CHAR,          /* a: the code point */
    RXI_CHAR_FOLD,     /* a: the folded code point */
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
    RXI_BACKREF,       /* a: the group; aux: case-insensitive */
    RXI_LOOK_ATOM,     /* aux, operand: the atom; aux: negate and behind */
    RXI_LOOKAHEAD,     /* a: the body's program; b: the continuation; aux: negate */
    RXI_LOOKBEHIND,    /* the same; operand: the length bounds */
    RXI_REPEAT_SIMPLE, /* aux, operand: the atom; a: min; b: max; aux: greedy; the continuation follows */
    RXI_REPEAT_ENTER,  /* slot: the counter to reset */
    RXI_REPEAT_LOOP,   /* slot; a: the exit; b: max; operand: min; aux: greedy; the body follows */
    RXI_REPEAT_NEXT,   /* slot; a: the loop; b: the exit */
    RXI_MATCH,         /* the pattern matched */
    RXI_MATCH_SUB,     /* a lookahead body matched */
    RXI_MATCH_AT       /* a lookbehind body matched, if it ends at the anchor */
} RxOp;

struct RxInst {
    uint8_t op;
    uint8_t aux;      /* an atom's kind and flags, a lookaround's negation, a repeat's greed */
    uint16_t slot;    /* the repeat counter slot */
    uint32_t a;
    uint32_t b;
    uint32_t operand; /* a class, alternation, or lookbehind index; an atom's code point */
};

/* An encoded single-code-point atom, in an instruction's aux (the kind) and operand */
#define RX_ATOM_CHAR      0
#define RX_ATOM_CHAR_FOLD 1
#define RX_ATOM_ANY       2
#define RX_ATOM_ANY_ALL   3
#define RX_ATOM_CLASS     4
#define RX_ATOM_KIND      0x07
#define RX_ATOM_FLAG      0x08 /* a simple repeat's greed; a lookaround atom's negation */
#define RX_LOOK_BEHIND    0x10

/* A repeat bound with no maximum */
#define RX_UNBOUNDED 0xFFFFFFFFu

/* An alternation, as the program keeps it */
typedef struct RxAlt {
    uint32_t count;
    uint32_t *branchPcs;    /* each alternative's program */
    RxBranchFirst *firsts;  /* per-alternative first sets, or NULL if none is usable */
    RxAltIndex *index;      /* the alternatives by first code point, for a wide alternation */
} RxAlt;

/* A lookbehind body's match length bounds */
typedef struct RxLook {
    size_t minLength;
    size_t maxLength;
} RxLook;


/* A backtrack entry - what to try next when the current path fails */
typedef enum {
    RX_BT_SPLIT,            /* resume at pc, pos */
    RX_BT_REPEAT_BODY,      /* enter the body of the repeat loop at pc */
    RX_BT_ALT_MASK,         /* the alternation at pc; aux: the alternatives still to try */
    RX_BT_ALT_INDEX,        /* the alternation at pc; aux: the next alternative to try */
    RX_BT_GIVEBACK,         /* the simple repeat before pc, greedy: give back to aux */
    RX_BT_GIVEBACK_LITERAL, /* the same, when a literal follows: only positions holding it */
    RX_BT_LAZY              /* the simple repeat before pc, lazy: take one more; aux: the start */
} RxBtKind;

typedef struct RxBacktrack {
    uint32_t kind;
    uint32_t pc;
    uint32_t pos;
    uint32_t trail;
    uint32_t aux;
    uint32_t unused;
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
    unsigned flags;
    BSRegexMatch *match;
    size_t end;
    long steps;
    RxTrailEntry *trail;
    size_t trailCount;
    size_t trailCapacity;
    RxTrailEntry trailInline[32];
    const RxInst *prog;
    const RxClass *classes;
    const RxAlt *alts;
    const RxLook *looks;
    RxBacktrack *bt;
    size_t btCount;
    size_t btCapacity;
    RxBacktrack btInline[64];
    RxRepeat *repeats;
    RxRepeat repeatsInline[16];
} RxState;


static inline uint32_t rxCode(const RxState *state, size_t pos)
{
    return state->codes != NULL ? state->codes[pos] : (uint32_t) state->bytes[pos];
}


static void rxTrailGrow(RxState *state)
{
    size_t capacity = state->trailCapacity * 2;
    RxTrailEntry *trail = bsAlloc(capacity * sizeof(RxTrailEntry));
    memcpy(trail, state->trail, state->trailCount * sizeof(RxTrailEntry));
    if (state->trail != state->trailInline) {
        free(state->trail);
    }
    state->trail = trail;
    state->trailCapacity = capacity;
}


static void rxTrailPush(RxState *state, size_t group)
{
    if (state->trailCount == state->trailCapacity) {
        rxTrailGrow(state);
    }
    RxTrailEntry *entry = &state->trail[state->trailCount++];
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


static bool rxIsSpaceCode(uint32_t ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == 0x0B ||
        ch == 0xA0 || ch == 0xFEFF || ch == 0x2028 || ch == 0x2029;
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
    if ((classes & RX_CLASS_SPACE) != 0 && rxIsSpaceCode(ch)) {
        return true;
    }
    if ((classes & RX_CLASS_NOTSPACE) != 0 && !rxIsSpaceCode(ch)) {
        return true;
    }
    for (size_t ix = 0; ix < cls->rangeCount; ix++) {
        if (ch >= cls->ranges[ix * 2] && ch <= cls->ranges[ix * 2 + 1]) {
            return true;
        }
    }
    return false;
}


static bool rxClassMatchSlow(const RxClass *cls, unsigned flags, uint32_t ch)
{
    bool matched = rxClassMatchOne(cls, ch);
    if (!matched && (flags & BS_REGEX_IGNORECASE) != 0) {
        uint32_t other = rxSwapCase(ch);
        if (other != ch) {
            matched = rxClassMatchOne(cls, other);
        }
    }
    return cls->negate ? !matched : matched;
}


/*
 * Precompute a class's membership for the code points 0 - 127, with the case-insensitivity flag
 * and negation applied, so an ASCII subject tests one bit instead of walking the ranges
 */
static void rxClassFinish(RxClass *cls, unsigned flags)
{
    memset(cls->ascii, 0, sizeof(cls->ascii));
    for (uint32_t ch = 0; ch < 128; ch++) {
        if (rxClassMatchSlow(cls, flags, ch)) {
            cls->ascii[ch >> 3] |= (uint8_t) (1u << (ch & 7));
        }
    }
}


static inline bool rxClassMatch(const RxState *state, const RxClass *cls, uint32_t ch)
{
    if (ch < 128) {
        return (cls->ascii[ch >> 3] >> (ch & 7)) & 1u;
    }
    return rxClassMatchSlow(cls, state->flags, ch);
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
    RxLook *looks;
    size_t lookCount;
    size_t lookCapacity;
    uint32_t repeatSlots;
    unsigned flags;
} RxEmit;


static uint32_t rxEmit(RxEmit *e, uint8_t op, uint32_t a, uint32_t b, uint32_t operand, uint8_t aux,
                       uint32_t slot)
{
    if (e->count == e->capacity) {
        e->capacity = e->capacity != 0 ? e->capacity * 2 : 32;
        e->inst = bsRealloc(e->inst, e->capacity * sizeof(RxInst));
    }
    RxInst *inst = &e->inst[e->count];
    inst->op = op;
    inst->aux = aux;
    inst->slot = (uint16_t) slot;
    inst->a = a;
    inst->b = b;
    inst->operand = operand;
    return (uint32_t) e->count++;
}


/* Move a class node's data into the program's class table. Returns the class index. */
static uint32_t rxEmitClass(RxEmit *e, RxNode *node)
{
    if (e->classCount == e->classCapacity) {
        e->classCapacity = e->classCapacity != 0 ? e->classCapacity * 2 : 8;
        e->classes = bsRealloc(e->classes, e->classCapacity * sizeof(RxClass));
    }
    e->classes[e->classCount] = node->u.cls;
    node->u.cls.ranges = NULL;
    return (uint32_t) e->classCount++;
}


/* Encode a single-code-point node as an atom kind and operand */
static uint32_t rxEmitAtom(RxEmit *e, RxNode *atom, unsigned *kind)
{
    switch (atom->kind) {
    case RX_CHAR:
        *kind = (e->flags & BS_REGEX_IGNORECASE) != 0 ? RX_ATOM_CHAR_FOLD : RX_ATOM_CHAR;
        return atom->u.ch;
    case RX_ANY:
        *kind = (e->flags & BS_REGEX_DOTALL) != 0 ? RX_ATOM_ANY_ALL : RX_ATOM_ANY;
        return 0;
    default:
        *kind = RX_ATOM_CLASS;
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
    RxBranchFirst *firsts = bsAlloc(count * sizeof(RxBranchFirst));
    bool usable = false;
    for (size_t ixBranch = 0; ixBranch < count; ixBranch++) {
        RxFirstSet set;
        memset(&set, 0, sizeof(set));
        bool nullable = rxFirstSet(node->u.alt.branches[ixBranch], flags, &set);
        RxBranchFirst *first = &firsts[ixBranch];
        memset(first, 0, sizeof(*first));
        first->usable = !nullable && !set.any;
        first->high = set.high;
        for (uint32_t code = 0; code < 256; code++) {
            if (set.codes[code]) {
                first->bits[code >> 6] |= (uint64_t) 1 << (code & 63);
            }
        }
        usable = usable || first->usable;
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
        const RxBranchFirst *first = &firsts[ixBranch];
        uint32_t bit = (uint32_t) 1 << ixBranch;
        if (!first->usable) {
            index->always |= bit;
            continue;
        }
        if (first->high) {
            index->high |= bit;
        }
        for (uint32_t code = 0; code < 256; code++) {
            if (rxBranchFirstHas(first, code)) {
                index->codes[code] |= bit;
            }
        }
    }
    alt->index = index;
}


static void rxEmitChain(RxEmit *e, RxNode *node)
{
    for (; node != NULL; node = node->next) {
        switch (node->kind) {
        case RX_CHAR:
            rxEmit(e, (e->flags & BS_REGEX_IGNORECASE) != 0 ? RXI_CHAR_FOLD : RXI_CHAR, node->u.ch, 0, 0, 0, 0);
            break;

        case RX_ANY:
            rxEmit(e, (e->flags & BS_REGEX_DOTALL) != 0 ? RXI_ANY_ALL : RXI_ANY, 0, 0, 0, 0, 0);
            break;

        case RX_CLASS:
            rxEmit(e, RXI_CLASS, 0, 0, rxEmitClass(e, node), 0, 0);
            break;

        case RX_ALT: {
            size_t count = node->u.alt.count;
            if (count == 1) {
                rxEmitChain(e, node->u.alt.branches[0]);
                break;
            }
            if (e->altCount == e->altCapacity) {
                e->altCapacity = e->altCapacity != 0 ? e->altCapacity * 2 : 8;
                e->alts = bsRealloc(e->alts, e->altCapacity * sizeof(RxAlt));
            }
            uint32_t altIndex = (uint32_t) e->altCount++;
            e->alts[altIndex].count = (uint32_t) count;
            rxEmitAltFirsts(&e->alts[altIndex], node, e->flags);
            rxEmit(e, RXI_ALT, 0, 0, altIndex, 0, 0);
            uint32_t *pcs = bsAlloc(count * sizeof(uint32_t));
            uint32_t *jumps = bsAlloc(count * sizeof(uint32_t));
            for (size_t ix = 0; ix < count; ix++) {
                pcs[ix] = (uint32_t) e->count;
                rxEmitChain(e, node->u.alt.branches[ix]);
                jumps[ix] = rxEmit(e, RXI_JMP, 0, 0, 0, 0, 0);
            }
            for (size_t ix = 0; ix < count; ix++) {
                e->inst[jumps[ix]].a = (uint32_t) e->count;
            }
            free(jumps);
            e->alts[altIndex].branchPcs = pcs;
            break;
        }

        case RX_GROUP:
            rxEmit(e, RXI_GROUP_BEGIN, (uint32_t) node->u.group.group, 0, 0, 0, 0);
            rxEmitChain(e, node->u.group.sub);
            rxEmit(e, RXI_GROUP_END, (uint32_t) node->u.group.group, 0, 0, 0, 0);
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
                rxEmit(e, RXI_REPEAT_SIMPLE, (uint32_t) node->u.repeat.min, maxOperand, operand,
                       (uint8_t) (kind | greedy), 0);
                break;
            }
            uint32_t slot = e->repeatSlots++;
            rxEmit(e, RXI_REPEAT_ENTER, 0, 0, 0, 0, slot);
            uint32_t loop = rxEmit(e, RXI_REPEAT_LOOP, 0, maxOperand, (uint32_t) node->u.repeat.min, greedy, slot);
            rxEmitChain(e, node->u.repeat.sub);
            uint32_t next = rxEmit(e, RXI_REPEAT_NEXT, loop, 0, 0, 0, slot);
            e->inst[loop].a = (uint32_t) e->count;
            e->inst[next].b = (uint32_t) e->count;
            break;
        }

        case RX_BOL:
            rxEmit(e, (e->flags & BS_REGEX_MULTILINE) != 0 ? RXI_BOL_ML : RXI_BOL, 0, 0, 0, 0, 0);
            break;

        case RX_EOL:
            rxEmit(e, (e->flags & BS_REGEX_MULTILINE) != 0 ? RXI_EOL_ML : RXI_EOL, 0, 0, 0, 0, 0);
            break;

        case RX_WORD_BOUNDARY:
            rxEmit(e, RXI_WB, 0, 0, 0, 0, 0);
            break;

        case RX_NOT_WORD_BOUNDARY:
            rxEmit(e, RXI_NWB, 0, 0, 0, 0, 0);
            break;

        case RX_BACKREF:
            rxEmit(e, RXI_BACKREF, (uint32_t) node->u.groupIndex, 0, 0,
                   (e->flags & BS_REGEX_IGNORECASE) != 0 ? 1 : 0, 0);
            break;

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
            uint32_t lookIndex = 0;
            if (!ahead) {
                /* A lookbehind's scan is bounded by its sub-pattern's match length */
                if (e->lookCount == e->lookCapacity) {
                    e->lookCapacity = e->lookCapacity != 0 ? e->lookCapacity * 2 : 4;
                    e->looks = bsRealloc(e->looks, e->lookCapacity * sizeof(RxLook));
                }
                lookIndex = (uint32_t) e->lookCount++;
                rxNodeLength(node->u.look.sub, &e->looks[lookIndex].minLength, &e->looks[lookIndex].maxLength);
            }
            uint32_t look = rxEmit(e, ahead ? RXI_LOOKAHEAD : RXI_LOOKBEHIND, 0, 0, lookIndex, negate, 0);
            e->inst[look].a = (uint32_t) e->count;
            rxEmitChain(e, node->u.look.sub);
            rxEmit(e, ahead ? RXI_MATCH_SUB : RXI_MATCH_AT, 0, 0, 0, 0, 0);
            e->inst[look].b = (uint32_t) e->count;
            break;
        }
        }
    }
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
    free(regex->looks);
    free(regex->prog);
}


static void rxEmitProgram(BSRegex *regex)
{
    RxEmit e;
    memset(&e, 0, sizeof(e));
    e.flags = regex->flags;
    rxEmitChain(&e, regex->root);
    rxEmit(&e, RXI_MATCH, 0, 0, 0, 0, 0);
    regex->prog = bsRealloc(e.inst, e.count * sizeof(RxInst));
    regex->progCount = e.count;
    regex->classes = e.classes;
    regex->classCount = e.classCount;
    regex->alts = e.alts;
    regex->altCount = e.altCount;
    regex->looks = e.looks;
    regex->lookCount = e.lookCount;
    regex->repeatCount = e.repeatSlots;
}


static void rxBtPush(RxState *state, uint32_t kind, uint32_t pc, size_t pos, uint32_t aux)
{
    if (state->btCount == state->btCapacity) {
        /* GCOV_EXCL_START - a memory guard the step budget reaches first */
        if (state->btCapacity >= RX_BACKTRACK_MAX) {
            state->steps = RX_STEPS_MAX;
            return;
        }
        /* GCOV_EXCL_STOP */
        size_t capacity = state->btCapacity * 2;
        RxBacktrack *bt = bsAlloc(capacity * sizeof(RxBacktrack));
        memcpy(bt, state->bt, state->btCount * sizeof(RxBacktrack));
        if (state->bt != state->btInline) {
            free(state->bt);
        }
        state->bt = bt;
        state->btCapacity = capacity;
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
    if (state->trailCount == state->trailCapacity) {
        rxTrailGrow(state);
    }
    RxTrailEntry *entry = &state->trail[state->trailCount++];
    entry->group = slot | RX_TRAIL_REPEAT;
    entry->span.begin = state->repeats[slot].count;
    entry->span.end = state->repeats[slot].start;
}


/* Whether an alternative can begin at a position, by its first set */
static inline bool rxAltViable(const RxAlt *alt, size_t ix, bool atEnd, uint32_t code)
{
    const RxBranchFirst *firsts = alt->firsts;
    if (firsts == NULL || !firsts[ix].usable) {
        return true;
    }
    return !atEnd && rxBranchFirstHas(&firsts[ix], code);
}


/* Whether an encoded single-code-point atom matches at a position */
static inline bool rxAtomAt(const RxState *state, unsigned kind, uint32_t operand, size_t pos)
{
    if (pos >= state->length) {
        return false;
    }
    uint32_t ch = rxCode(state, pos);
    switch (kind) {
    case RX_ATOM_CHAR:
        return ch == operand;
    case RX_ATOM_CHAR_FOLD:
        return rxFold(ch) == operand;
    case RX_ATOM_ANY:
        return ch != '\n' && ch != '\r' && ch != 0x2028 && ch != 0x2029;
    case RX_ATOM_ANY_ALL:
        return true;
    default:
        return rxClassMatch(state, &state->classes[operand], ch);
    }
}


/*
 * Run the program from "pc" at "pos". A lookahead body runs to RXI_MATCH_SUB; a lookbehind body
 * runs to RXI_MATCH_AT, which requires it to end at "anchor". Returns whether the program
 * matched - the pattern's end position is left in state->end - with the backtrack entries the
 * run pushed discarded either way.
 */
static bool rxRun(RxState *state, uint32_t startPc, size_t startPos, size_t anchor)
{
    const RxInst *prog = state->prog;
    const size_t length = state->length;
    const size_t btBase = state->btCount;
    uint32_t pc = startPc;
    size_t pos = startPos;

#ifdef RX_THREADED_DISPATCH
    /* Indexed by opcode - the order is the RxOp enumeration's */
    static const void *const dispatch[] = {
        &&op_CHAR, &&op_CHAR_FOLD, &&op_ANY, &&op_ANY_ALL, &&op_CLASS, &&op_ALT, &&op_JMP,
        &&op_GROUP_BEGIN, &&op_GROUP_END, &&op_BOL, &&op_BOL_ML, &&op_EOL, &&op_EOL_ML, &&op_WB,
        &&op_NWB, &&op_BACKREF, &&op_LOOK_ATOM, &&op_LOOKAHEAD, &&op_LOOKBEHIND, &&op_REPEAT_SIMPLE,
        &&op_REPEAT_ENTER, &&op_REPEAT_LOOP, &&op_REPEAT_NEXT, &&op_MATCH, &&op_MATCH_SUB, &&op_MATCH_AT
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
            if (pos >= length || rxCode(state, pos) != inst->a) {
                goto backtrack;
            }
            pos++;
            pc++;
            RX_NEXT();

        RX_CASE(CHAR_FOLD)
            if (pos >= length || rxFold(rxCode(state, pos)) != inst->a) {
                goto backtrack;
            }
            pos++;
            pc++;
            RX_NEXT();

        RX_CASE(ANY) {
            if (pos >= length) {
                goto backtrack;
            }
            uint32_t ch = rxCode(state, pos);
            if (ch == '\n' || ch == '\r' || ch == 0x2028 || ch == 0x2029) {
                goto backtrack;
            }
            pos++;
            pc++;
        }
        RX_NEXT();

        RX_CASE(ANY_ALL)
            if (pos >= length) {
                goto backtrack;
            }
            pos++;
            pc++;
            RX_NEXT();

        RX_CASE(CLASS)
            if (pos >= length || !rxClassMatch(state, &state->classes[inst->operand], rxCode(state, pos))) {
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
                uint32_t mask;
                const RxAltIndex *index = alt->index;
                if (index != NULL) {
                    mask = index->always | (atEnd ? 0 : (code < 256 ? index->codes[code] : index->high));
                } else {
                    mask = 0;
                    for (size_t ix = 0; ix < count; ix++) {
                        if (rxAltViable(alt, ix, atEnd, code)) {
                            mask |= (uint32_t) 1 << ix;
                        }
                    }
                }
                if (mask == 0) {
                    goto backtrack;
                }
                unsigned ix = rxLowestBit(mask);
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

        RX_CASE(BOL)
            if (pos != 0) {
                goto backtrack;
            }
            pc++;
            RX_NEXT();

        RX_CASE(BOL_ML)
            if (pos != 0 && rxCode(state, pos - 1) != '\n') {
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
            if (pos != length && rxCode(state, pos) != '\n') {
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
            if (group < state->match->groupCount && state->match->matched[group]) {
                BSRegexSpan span = state->match->groups[group];
                size_t size = span.end - span.begin;
                if (pos + size > length) {
                    goto backtrack;
                }
                for (size_t ix = 0; ix < size; ix++) {
                    uint32_t expected = rxCode(state, span.begin + ix);
                    uint32_t actual = rxCode(state, pos + ix);
                    if (inst->aux != 0 ? rxFold(expected) != rxFold(actual) : expected != actual) {
                        goto backtrack;
                    }
                }
                pos += size;
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

        RX_CASE(LOOKAHEAD) {
            size_t mark = state->trailCount;
            bool matched = rxRun(state, inst->a, pos, 0);
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

        RX_CASE(LOOKBEHIND) {
            /*
             * Try every start position whose distance from "pos" is a possible body match length,
             * requiring the body to end exactly at "pos"
             */
            const RxLook *look = &state->looks[inst->operand];
            size_t mark = state->trailCount;
            size_t maxLength = look->maxLength > pos ? pos : look->maxLength;
            bool matched = false;
            for (size_t len = look->minLength; len <= maxLength && !matched; len++) {
                matched = rxRun(state, inst->a, pos - len, pos);
                if (!matched) {
                    rxTrailUnwind(state, mark);
                }
            }
            bool negate = inst->aux != 0;
            if (negate) {
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
            size_t limit = (max == RX_UNBOUNDED || max > length - pos) ? length : pos + max;
            uint32_t next = pc + 1;
            size_t end = pos;

            if ((inst->aux & RX_ATOM_FLAG) == 0) {
                /* Lazy: take the minimum, then one more each time the continuation fails */
                size_t count = 0;
                while (count < min && rxAtomAt(state, kind, operand, end)) {
                    end++;
                    count++;
                }
                if (count < min) {
                    goto backtrack;
                }
                if (max == RX_UNBOUNDED || count < max) {
                    rxBtPush(state, RX_BT_LAZY, next, end, (uint32_t) pos);
                }
                pos = end;
                pc = next;
                goto dispatch;
            }

            /* Consume as much as the body matches - an ASCII subject scans by the body's kind */
            if (state->codes == NULL) {
                const unsigned char *bytes = state->bytes;
                switch (kind) {
                case RX_ATOM_CHAR:
                    while (end < limit && bytes[end] == operand) {
                        end++;
                    }
                    break;
                case RX_ATOM_CHAR_FOLD:
                    while (end < limit && rxFold(bytes[end]) == operand) {
                        end++;
                    }
                    break;
                case RX_ATOM_ANY:
                    while (end < limit && bytes[end] != '\n' && bytes[end] != '\r') {
                        end++;
                    }
                    break;
                case RX_ATOM_ANY_ALL:
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
            bool literal = prog[next].op == RXI_CHAR;
            if (literal) {
                uint32_t ch = prog[next].a;
                while (!(end < length && rxCode(state, end) == ch)) {
                    if (end == stop) {
                        goto backtrack;
                    }
                    end--;
                }
            }
            if (end > stop) {
                rxBtPush(state, literal ? RX_BT_GIVEBACK_LITERAL : RX_BT_GIVEBACK, next, end, (uint32_t) stop);
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
            bool enter = true;
            if (inst->b != RX_UNBOUNDED && count >= inst->b) {
                pc = inst->a;
                enter = false;
            } else if (count >= inst->operand) {
                if (inst->aux == 0) {
                    /* Lazy: try the continuation first; the body is the alternative */
                    rxBtPush(state, RX_BT_REPEAT_BODY, pc, pos, 0);
                    pc = inst->a;
                    enter = false;
                } else {
                    rxBtPush(state, RX_BT_SPLIT, inst->a, pos, 0);
                }
            }
            if (enter) {
                rxTrailPushRepeat(state, slot);
                state->repeats[slot].start = pos;
                state->repeats[slot].count = count + 1;
                pc++;
            }
        }
        RX_NEXT();

        RX_CASE(REPEAT_NEXT)
            /* An iteration that consumed nothing ends the repetition */
            pc = pos == state->repeats[inst->slot].start ? inst->b : inst->a;
            RX_NEXT();

        RX_CASE(MATCH)
            state->end = pos;
            state->btCount = btBase;
            return true;

        RX_CASE(MATCH_SUB)
            state->btCount = btBase;
            return true;

        RX_CASE(MATCH_AT)
            if (pos != anchor) {
                goto backtrack;
            }
            state->btCount = btBase;
            return true;

#ifndef RX_THREADED_DISPATCH
        }
#endif
    dispatch:
        RX_NEXT();

    backtrack:
        for (;;) {
            if (state->btCount == btBase) {
                return false;
            }
            if (++state->steps > RX_STEPS_MAX) {
                state->btCount = btBase;
                return false;
            }
            RxBacktrack *bt = &state->bt[state->btCount - 1];
            switch (bt->kind) {

            case RX_BT_SPLIT:
                pc = bt->pc;
                pos = bt->pos;
                state->btCount--;
                break;

            case RX_BT_REPEAT_BODY: {
                uint32_t slot = prog[bt->pc].slot;
                pc = bt->pc + 1;
                pos = bt->pos;
                rxTrailUnwind(state, bt->trail);
                state->btCount--;
                rxTrailPushRepeat(state, slot);
                state->repeats[slot].start = pos;
                state->repeats[slot].count++;
                goto resume;
            }

            case RX_BT_ALT_MASK: {
                uint32_t mask = bt->aux;
                unsigned ix = rxLowestBit(mask);
                mask &= mask - 1;
                pc = state->alts[prog[bt->pc].operand].branchPcs[ix];
                pos = bt->pos;
                if (mask != 0) {
                    bt->aux = mask;
                    rxTrailUnwind(state, bt->trail);
                    goto resume;
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
                    rxTrailUnwind(state, bt->trail);
                    goto resume;
                }
                state->btCount--;
                break;
            }

            case RX_BT_GIVEBACK:
            case RX_BT_GIVEBACK_LITERAL: {
                /* The entry is only kept while there is more to give back, so end is past stop */
                size_t end = bt->pos - 1;
                size_t stop = bt->aux;
                if (bt->kind == RX_BT_GIVEBACK_LITERAL) {
                    uint32_t ch = prog[bt->pc].a;
                    while (!(end < length && rxCode(state, end) == ch)) {
                        if (end == stop) {
                            state->btCount--;
                            goto backtrack;
                        }
                        end--;
                    }
                }
                pc = bt->pc;
                pos = end;
                if (end > stop) {
                    bt->pos = (uint32_t) end;
                    rxTrailUnwind(state, bt->trail);
                    goto resume;
                }
                state->btCount--;
                break;
            }

            default: {
                /* RX_BT_LAZY - one more of the body, while the bound and the body allow */
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
                rxTrailUnwind(state, bt->trail);
                goto resume;
            }
            }

            /* A popped entry - its trail mark is where the next attempt starts */
            rxTrailUnwind(state, state->bt[state->btCount].trail);
        resume:
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
    subject->owned = NULL;
    subject->codes = NULL;
    subject->bytes = NULL;
}


bool bsRegexSearch(BSValue regex, const BSRegexSubject *subject, size_t start, BSRegexMatch *match)
{
    BSRegex *compiled = regex.u.regex;
    RxState state;
    state.codes = subject->codes;
    state.bytes = subject->bytes;
    state.length = subject->length;
    state.flags = compiled->flags;
    state.match = match;
    state.trail = state.trailInline;
    state.trailCapacity = sizeof(state.trailInline) / sizeof(state.trailInline[0]);
    state.prog = compiled->prog;
    state.classes = compiled->classes;
    state.alts = compiled->alts;
    state.looks = compiled->looks;
    state.bt = state.btInline;
    state.btCapacity = sizeof(state.btInline) / sizeof(state.btInline[0]);
    state.btCount = 0;
    state.repeats = compiled->repeatCount <= sizeof(state.repeatsInline) / sizeof(state.repeatsInline[0]) ?
        state.repeatsInline : bsAlloc(compiled->repeatCount * sizeof(RxRepeat));

    /*
     * Only the pattern's own groups need clearing, and only their matched flags - a span is read
     * only once its flag is set - and only once: every capture write is on the trail, and a failed
     * attempt unwinds all of them.
     */
    memset(match->matched, 0, compiled->groupCount * sizeof(bool));
    match->begin = 0;
    match->end = 0;
    match->groupCount = compiled->groupCount;
    size_t last = compiled->anchored ? start : subject->length;
    bool found = false;
    for (size_t pos = start; pos <= last && !found; pos++) {
        /* Skip positions whose code point cannot begin a match */
        if (compiled->firstUsable) {
            while (pos < subject->length) {
                uint32_t code = subject->codes != NULL ? subject->codes[pos] :
                    (uint32_t) subject->bytes[pos];
                if (code >= 256 ? compiled->first.high : compiled->first.codes[code]) {
                    break;
                }
                pos++;
            }
            if (pos >= subject->length) {
                break;
            }
        }
        state.steps = 0;
        state.trailCount = 0;
        state.btCount = 0;
        found = rxRun(&state, 0, pos, 0);
        if (found) {
            match->begin = pos;
            match->end = state.end;
            match->groups[0].begin = pos;
            match->groups[0].end = state.end;
            match->matched[0] = true;
        }
    }
    if (state.trail != state.trailInline) {
        free(state.trail);
    }
    if (state.bt != state.btInline) {
        free(state.bt);
    }
    if (state.repeats != state.repeatsInline) {
        free(state.repeats);
    }
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
        if (ch != '\0' && (unsigned char) ch < 0x80 && strchr(special, ch) != NULL) {
            bsSBAppendChar(&sb, '\\');
        }
        bsSBAppendChar(&sb, ch);
    }
    return bsSBToValue(&sb);
}
