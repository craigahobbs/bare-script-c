/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * A targeted regular expression implementation for BareScript
 *
 * Patterns compile to a tree of nodes chained by "next" pointers. Matching is a backtracking walk
 * of that tree with an explicit continuation list, so alternation, groups, and quantifiers each
 * stay a small local decision. Quantifiers whose body matches exactly one code point - the common
 * case, "\\s*", "[0-9]+", ".*" - are matched iteratively rather than recursively, which keeps the
 * C stack bounded for the long subject strings that make up most real input.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "barescript/regex.h"

#include "internal.h"


/* The maximum backtracking recursion depth */
#define RX_DEPTH_MAX 5000

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
    RX_GROUP_END,
    RX_REPEAT,
    RX_BOL,
    RX_EOL,
    RX_WORD_BOUNDARY,
    RX_NOT_WORD_BOUNDARY,
    RX_BACKREF,
    RX_LOOKAHEAD,
    RX_LOOKBEHIND
} RxKind;


typedef struct RxNode RxNode;

struct RxNode {
    RxKind kind;
    RxNode *next;
    union {
        uint32_t ch;
        struct {
            uint32_t *ranges; /* pairs of inclusive code point bounds */
            size_t rangeCount;
            unsigned classes;
            bool negate;
        } cls;
        struct {
            RxNode **branches;
            size_t count;
        } alt;
        struct {
            RxNode *sub;
            RxNode *close;
            size_t group; /* zero for a non-capturing group */
        } group;
        size_t groupIndex; /* RX_GROUP_END and RX_BACKREF */
        struct {
            RxNode *sub;
            int min;
            int max; /* -1 for unbounded */
            bool greedy;
            bool simple; /* the body matches exactly one code point and captures nothing */
        } repeat;
        struct {
            RxNode *sub;
            bool negate;
            size_t minLength; /* the sub-pattern's match length bounds, for lookbehind scanning */
            size_t maxLength;
        } look;
    } u;
};


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


struct BSRegex {
    int32_t refcount;
    BSValue pattern;
    unsigned flags;
    bool anchored; /* every alternative begins with "^", so only the start position can match */
    bool firstUsable;
    RxFirstSet first;
    RxNode *root;
    size_t groupCount;
    BSValue groupNames[BS_REGEX_GROUPS_MAX];
    RxNode **nodes;
    size_t nodeCount;
    size_t nodeCapacity;
};


/*
 * Compile
 */


typedef struct RxCompiler {
    const char *pattern;
    size_t size;
    size_t offset;
    unsigned flags;
    BSRegex *regex;
    char *error;      /* the caller's message buffer, empty until a failure */
    size_t errorSize;
    bool failed;
} RxCompiler;


static bool rxIsNameChar(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
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
    while (end < size && rxIsNameChar(pattern[end])) {
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
    if (regex->nodeCount == regex->nodeCapacity) {
        regex->nodeCapacity = regex->nodeCapacity != 0 ? regex->nodeCapacity * 2 : 16;
        regex->nodes = bsRealloc(regex->nodes, regex->nodeCapacity * sizeof(RxNode *));
    }
    RxNode *node = bsAlloc(sizeof(RxNode));
    memset(node, 0, sizeof(RxNode));
    node->kind = kind;
    regex->nodes[regex->nodeCount++] = node;
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


static bool rxHexValue(char ch, uint32_t *digit)
{
    if (ch >= '0' && ch <= '9') {
        *digit = (uint32_t) (ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
        *digit = (uint32_t) (ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
        *digit = (uint32_t) (ch - 'A' + 10);
    } else {
        return false;
    }
    return true;
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
        uint32_t digit;
        if (!rxHexValue(compiler->pattern[compiler->offset + digits], &digit)) {
            break;
        }
        value = (value << 4) | digit;
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
 * Parse an escape sequence. Returns the predefined class flag, or zero for a literal code point,
 * which is stored in "*literal". Returns SIZE_MAX-style failure through compiler->error.
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


static RxNode *rxParseClass(RxCompiler *compiler, size_t classOffset)
{
    RxNode *node = rxNodeNew(compiler, RX_CLASS);
    if (compiler->offset < compiler->size && compiler->pattern[compiler->offset] == '^') {
        node->u.cls.negate = true;
        compiler->offset++;
    }

    bool first = true;
    while (compiler->offset < compiler->size) {
        char ch = compiler->pattern[compiler->offset];
        if (ch == ']' && !first) {
            compiler->offset++;
            return node;
        }
        if (ch == ']' && first) {
            /* An empty class - "[]" - matches nothing */
            compiler->offset++;
            return node;
        }
        first = false;

        /* The range's low bound */
        size_t lowOffset = compiler->offset;
        uint32_t lo;
        if (ch == '\\') {
            compiler->offset++;
            if (compiler->offset >= compiler->size) {
                rxError(compiler, compiler->offset - 1, "bad escape (end of pattern)");
                return NULL;
            }
            /* "\b" is a backspace inside a character class */
            if (compiler->pattern[compiler->offset] == 'b') {
                compiler->offset++;
                lo = '\b';
            } else {
                unsigned classes = rxEscape(compiler, &lo);
                if (compiler->failed) {
                    return NULL;
                }
                if (classes != 0) {
                    node->u.cls.classes |= classes;
                    continue;
                }
            }
        } else {
            size_t codeSize;
            lo = bsUTF8Decode(compiler->pattern, compiler->size, compiler->offset, &codeSize);
            compiler->offset += codeSize;
        }

        /* The optional range's high bound */
        size_t lowEnd = compiler->offset;
        uint32_t hi = lo;
        if (compiler->offset + 1 < compiler->size && compiler->pattern[compiler->offset] == '-' &&
            compiler->pattern[compiler->offset + 1] != ']') {
            compiler->offset++;
            size_t highOffset = compiler->offset;
            char next = compiler->pattern[compiler->offset];
            bool badRange = false;
            if (next == '\\') {
                compiler->offset++;
                if (compiler->offset >= compiler->size) {
                    rxError(compiler, compiler->offset - 1, "bad escape (end of pattern)");
                    return NULL;
                }
                unsigned classes = rxEscape(compiler, &hi);
                if (compiler->failed) {
                    return NULL;
                }
                badRange = (classes != 0);
            } else {
                size_t codeSize;
                hi = bsUTF8Decode(compiler->pattern, compiler->size, compiler->offset, &codeSize);
                compiler->offset += codeSize;
            }
            if (badRange || hi < lo) {
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
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) {
            return false;
        }
        compiler->offset++;
    }
    if (compiler->offset >= compiler->size || compiler->offset == begin) {
        return false;
    }
    *name = bsStringNewSize(compiler->pattern + begin, compiler->offset - begin);
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
            compiler->regex->groupNames[group] = name;
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
        node->u.group.close = rxNodeNew(compiler, RX_GROUP_END);
        node->u.group.close->u.groupIndex = group;
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
        compiler->offset++;
        if (compiler->offset >= compiler->size) {
            rxError(compiler, escapeOffset, "bad escape (end of pattern)");
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
            size_t group = 0;
            while (compiler->offset < compiler->size && compiler->pattern[compiler->offset] >= '0' &&
                   compiler->pattern[compiler->offset] <= '9') {
                group = group * 10 + (size_t) (compiler->pattern[compiler->offset] - '0');
                compiler->offset++;
            }
            if (group >= BS_REGEX_GROUPS_MAX) {
                rxError(compiler, digitOffset, "invalid group reference %zu", group);
                return NULL;
            }
            RxNode *node = rxNodeNew(compiler, RX_BACKREF);
            node->u.groupIndex = group;
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
                if (compiler->regex->groupNames[ix].type == BS_STRING &&
                    bsValueCompare(compiler->regex->groupNames[ix], name) == 0) {
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
            return node;
        }
        RxNode *node = rxNodeNew(compiler, RX_CHAR);
        node->u.ch = (compiler->flags & BS_REGEX_IGNORECASE) != 0 ? rxFold(literal) : literal;
        return node;
    }

    /* A literal code point */
    size_t codeSize;
    uint32_t literal = bsUTF8Decode(compiler->pattern, compiler->size, compiler->offset, &codeSize);
    compiler->offset += codeSize;
    RxNode *node = rxNodeNew(compiler, RX_CHAR);
    node->u.ch = (compiler->flags & BS_REGEX_IGNORECASE) != 0 ? rxFold(literal) : literal;
    return node;
}


/* True if a node matches exactly one code point and captures nothing */
static bool rxIsSimple(const RxNode *node)
{
    return node->next == NULL && (node->kind == RX_CHAR || node->kind == RX_ANY || node->kind == RX_CLASS);
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
    size_t digits = 0;
    *min = 0;
    while (compiler->offset < compiler->size && compiler->pattern[compiler->offset] >= '0' &&
           compiler->pattern[compiler->offset] <= '9') {
        *min = *min * 10 + (compiler->pattern[compiler->offset] - '0');
        compiler->offset++;
        digits++;
    }
    if (digits == 0) {
        compiler->offset = save;
        return false;
    }
    *max = *min;
    if (compiler->offset < compiler->size && compiler->pattern[compiler->offset] == ',') {
        compiler->offset++;
        size_t maxDigits = 0;
        *max = 0;
        while (compiler->offset < compiler->size && compiler->pattern[compiler->offset] >= '0' &&
               compiler->pattern[compiler->offset] <= '9') {
            *max = *max * 10 + (compiler->pattern[compiler->offset] - '0');
            compiler->offset++;
            maxDigits++;
        }
        if (maxDigits == 0) {
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
            repeat->u.repeat.simple = rxIsSimple(atom);
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
    if (length == RX_LENGTH_MAX || count < 0) {
        return length == 0 ? 0 : RX_LENGTH_MAX;
    }
    return length * (size_t) count;
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
            /* RX_ANY, RX_BACKREF, and RX_GROUP_END - not worth enumerating */
            set->any = true;
            return false;
        }
    }
    return true;
}


static void bsRegexFree(BSRegex *regex)
{
    for (size_t ix = 0; ix < regex->nodeCount; ix++) {
        RxNode *node = regex->nodes[ix];
        if (node->kind == RX_CLASS) {
            free(node->u.cls.ranges);
        } else if (node->kind == RX_ALT) {
            free(node->u.alt.branches);
        }
        free(node);
    }
    free(regex->nodes);
    for (size_t ix = 0; ix < regex->groupCount; ix++) {
        bsRelease(regex->groupNames[ix]);
    }
    bsRelease(regex->pattern);
    free(regex);
}


BSValue bsRegexNew(const char *pattern, size_t patternSize, unsigned flags, char *error,
                   size_t errorSize)
{
    if (error != NULL && errorSize != 0) {
        error[0] = '\0';
    }
    BSRegex *regex = bsAlloc(sizeof(BSRegex));
    regex->refcount = 1;
    regex->pattern = bsStringNewSize(pattern, patternSize);
    regex->flags = flags;
    regex->anchored = false;
    regex->firstUsable = false;
    memset(&regex->first, 0, sizeof(regex->first));
    regex->root = NULL;
    regex->groupCount = 1;
    regex->nodes = NULL;
    regex->nodeCount = 0;
    regex->nodeCapacity = 0;
    for (size_t ix = 0; ix < BS_REGEX_GROUPS_MAX; ix++) {
        regex->groupNames[ix] = bsNull();
    }

    RxCompiler compiler = {pattern, patternSize, 0, flags, regex, error, errorSize, false};
    regex->root = rxParseAlternation(&compiler);
    if (!compiler.failed && compiler.offset != patternSize) {
        rxError(&compiler, compiler.offset, "unbalanced parenthesis");
    }
    if (!compiler.failed) {
        /* A group's close node continues where the group itself continues */
        for (size_t ix = 0; ix < regex->nodeCount; ix++) {
            RxNode *node = regex->nodes[ix];
            if (node->kind == RX_GROUP) {
                node->u.group.close->next = node->next;
            }
        }

        /*
         * A pattern whose every alternative begins with "^" can only match at the search start, so
         * the scan over later positions is skipped. Multi-line patterns still scan, since "^" also
         * matches after a newline. The parser's patterns are all anchored, so this is the
         * difference between a linear and a quadratic scan over every line it parses.
         */
        if ((flags & BS_REGEX_MULTILINE) == 0 && regex->root->kind == RX_ALT) {
            regex->anchored = regex->root->u.alt.count != 0;
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

        /* Bound each lookbehind's scan by its sub-pattern's match length */
        for (size_t ix = 0; ix < regex->nodeCount; ix++) {
            RxNode *node = regex->nodes[ix];
            if (node->kind == RX_LOOKBEHIND) {
                rxNodeLength(node->u.look.sub, &node->u.look.minLength, &node->u.look.maxLength);
            }
        }
    }
    if (compiler.failed) {
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


const char *bsRegexPattern(BSValue regex)
{
    return bsStringData(regex.u.regex->pattern);
}


unsigned bsRegexFlags(BSValue regex)
{
    return regex.u.regex->flags;
}


size_t bsRegexGroupCount(BSValue regex)
{
    return regex.u.regex->groupCount;
}


const char *bsRegexGroupName(BSValue regex, size_t group)
{
    if (group >= regex.u.regex->groupCount || regex.u.regex->groupNames[group].type != BS_STRING) {
        return NULL;
    }
    return bsStringData(regex.u.regex->groupNames[group]);
}


/*
 * Match
 */


typedef enum {
    RX_CONT_NODE,
    RX_CONT_REPEAT,
    RX_CONT_END,
    RX_CONT_STOP,
    RX_CONT_ANCHOR
} RxContKind;


typedef struct RxCont {
    RxContKind kind;
    RxNode *node;
    int count;      /* the repeat iteration count */
    size_t startPos;/* the repeat iteration's start position */
    struct RxCont *next;
} RxCont;


/*
 * A capture trail entry
 *
 * Every capture group write records its previous value, so backtracking - and in particular
 * lookaround, which can write many groups before failing - restores state in time proportional to
 * what actually changed rather than copying the whole capture array.
 */
typedef struct RxTrailEntry {
    size_t group;
    BSRegexSpan span;
    bool matched;
} RxTrailEntry;


typedef struct RxState {
    const uint32_t *codes;
    const unsigned char *bytes;
    size_t length;
    unsigned flags;
    BSRegex *regex;
    BSRegexMatch *match;
    size_t end;
    int depth;
    long steps;
    RxTrailEntry *trail;
    size_t trailCount;
    size_t trailCapacity;
    RxTrailEntry trailInline[32];
} RxState;


static inline uint32_t rxCode(const RxState *state, size_t pos)
{
    return state->codes != NULL ? state->codes[pos] : (uint32_t) state->bytes[pos];
}


static void rxTrailPush(RxState *state, size_t group)
{
    if (state->trailCount == state->trailCapacity) {
        size_t capacity = state->trailCapacity * 2;
        RxTrailEntry *trail = bsAlloc(capacity * sizeof(RxTrailEntry));
        memcpy(trail, state->trail, state->trailCount * sizeof(RxTrailEntry));
        if (state->trail != state->trailInline) {
            free(state->trail);
        }
        state->trail = trail;
        state->trailCapacity = capacity;
    }
    RxTrailEntry *entry = &state->trail[state->trailCount++];
    entry->group = group;
    entry->span = state->match->groups[group];
    entry->matched = state->match->matched[group];
}


static void rxTrailUnwind(RxState *state, size_t mark)
{
    while (state->trailCount > mark) {
        RxTrailEntry *entry = &state->trail[--state->trailCount];
        state->match->groups[entry->group] = entry->span;
        state->match->matched[entry->group] = entry->matched;
    }
}


static bool rxIsWordCode(uint32_t ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
}


static bool rxIsSpaceCode(uint32_t ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == 0x0B ||
        ch == 0xA0 || ch == 0xFEFF || ch == 0x2028 || ch == 0x2029;
}


static bool rxClassMatchOne(const RxNode *node, uint32_t ch)
{
    unsigned classes = node->u.cls.classes;
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
    for (size_t ix = 0; ix < node->u.cls.rangeCount; ix++) {
        if (ch >= node->u.cls.ranges[ix * 2] && ch <= node->u.cls.ranges[ix * 2 + 1]) {
            return true;
        }
    }
    return false;
}


static bool rxClassMatch(const RxState *state, const RxNode *node, uint32_t ch)
{
    bool matched = rxClassMatchOne(node, ch);
    if (!matched && (state->flags & BS_REGEX_IGNORECASE) != 0) {
        uint32_t other = rxSwapCase(ch);
        if (other != ch) {
            matched = rxClassMatchOne(node, other);
        }
    }
    return node->u.cls.negate ? !matched : matched;
}


/* An alternation of one single-code-point atom, or that atom itself */
static const RxNode *rxSimpleAtom(const RxNode *node)
{
    if (node != NULL && node->kind == RX_ALT && node->u.alt.count == 1 && node->next == NULL) {
        node = node->u.alt.branches[0];
    }
    return (node != NULL && rxIsSimple(node)) ? node : NULL;
}


/* Match a single-code-point node at a position */
static bool rxMatchOne(const RxState *state, const RxNode *node, size_t pos)
{
    if (pos >= state->length) {
        return false;
    }
    uint32_t ch = rxCode(state, pos);
    switch (node->kind) {
    case RX_CHAR:
        return (state->flags & BS_REGEX_IGNORECASE) != 0 ? rxFold(ch) == node->u.ch : ch == node->u.ch;
    case RX_ANY:
        return (state->flags & BS_REGEX_DOTALL) != 0 || (ch != '\n' && ch != '\r' && ch != 0x2028 && ch != 0x2029);
    default:
        return rxClassMatch(state, node, ch);
    }
}


/* ASCII subjects: code point i is bytes[i], and U+2028 / U+2029 cannot appear */
static bool rxMatchOneByte(const RxState *state, const RxNode *node, size_t pos)
{
    if (pos >= state->length) {
        return false;
    }
    uint32_t ch = state->bytes[pos];
    switch (node->kind) {
    case RX_CHAR:
        return (state->flags & BS_REGEX_IGNORECASE) != 0 ? rxFold(ch) == node->u.ch : ch == node->u.ch;
    case RX_ANY:
        return (state->flags & BS_REGEX_DOTALL) != 0 || (ch != '\n' && ch != '\r');
    default:
        return rxClassMatch(state, node, ch);
    }
}


static bool rxMatchNode(RxState *state, RxNode *node, RxCont *cont, size_t pos);


static bool rxMatchCont(RxState *state, RxCont *cont, size_t pos)
{
    if (cont->kind == RX_CONT_END) {
        state->end = pos;
        return true;
    }
    if (cont->kind == RX_CONT_STOP) {
        return true;
    }
    if (cont->kind == RX_CONT_ANCHOR) {
        return pos == cont->startPos;
    }
    if (cont->kind == RX_CONT_NODE) {
        return rxMatchNode(state, cont->node, cont->next, pos);
    }

    /* A repeat continuation - an iteration that consumed nothing ends the repetition */
    RxNode *repeat = cont->node;
    if (pos == cont->startPos) {
        return rxMatchNode(state, repeat->next, cont->next, pos);
    }
    int count = cont->count;
    if (repeat->u.repeat.max >= 0 && count >= repeat->u.repeat.max) {
        return rxMatchNode(state, repeat->next, cont->next, pos);
    }
    if (count < repeat->u.repeat.min) {
        RxCont iteration = {RX_CONT_REPEAT, repeat, count + 1, pos, cont->next};
        return rxMatchNode(state, repeat->u.repeat.sub, &iteration, pos);
    }
    if (repeat->u.repeat.greedy) {
        RxCont iteration = {RX_CONT_REPEAT, repeat, count + 1, pos, cont->next};
        if (rxMatchNode(state, repeat->u.repeat.sub, &iteration, pos)) {
            return true;
        }
        return rxMatchNode(state, repeat->next, cont->next, pos);
    }
    if (rxMatchNode(state, repeat->next, cont->next, pos)) {
        return true;
    }
    RxCont iteration = {RX_CONT_REPEAT, repeat, count + 1, pos, cont->next};
    return rxMatchNode(state, repeat->u.repeat.sub, &iteration, pos);
}


static bool rxMatchSimpleRepeat(RxState *state, RxNode *node, RxCont *cont, size_t pos)
{
    RxNode *sub = node->u.repeat.sub;
    int min = node->u.repeat.min;
    int max = node->u.repeat.max;
    bool ascii = state->codes == NULL;

    if (node->u.repeat.greedy) {
        size_t end = pos;
        int count = 0;
        if (ascii) {
            while ((max < 0 || count < max) && rxMatchOneByte(state, sub, end)) {
                end++;
                count++;
            }
        } else {
            while ((max < 0 || count < max) && rxMatchOne(state, sub, end)) {
                end++;
                count++;
            }
        }
        if (count < min) {
            return false;
        }
        while (count >= min) {
            if (rxMatchNode(state, node->next, cont, end)) {
                return true;
            }
            if (count == 0) {
                break;
            }
            count--;
            end--;
        }
        return false;
    }

    size_t end = pos;
    int count = 0;
    while (count < min) {
        if (ascii ? !rxMatchOneByte(state, sub, end) : !rxMatchOne(state, sub, end)) {
            return false;
        }
        end++;
        count++;
    }
    while (true) {
        if (rxMatchNode(state, node->next, cont, end)) {
            return true;
        }
        if ((max >= 0 && count >= max) ||
            (ascii ? !rxMatchOneByte(state, sub, end) : !rxMatchOne(state, sub, end))) {
            return false;
        }
        end++;
        count++;
    }
}


static bool rxMatchNode(RxState *state, RxNode *node, RxCont *cont, size_t pos)
{
    if (node == NULL) {
        return rxMatchCont(state, cont, pos);
    }
    if (++state->depth > RX_DEPTH_MAX || ++state->steps > RX_STEPS_MAX) {
        state->depth--;
        return false;
    }

    bool result = false;
    switch (node->kind) {
    case RX_CHAR:
    case RX_ANY:
    case RX_CLASS: {
        bool matched = true;
        if (state->codes == NULL) {
            while (node != NULL &&
                   (node->kind == RX_CHAR || node->kind == RX_ANY || node->kind == RX_CLASS)) {
                if (!rxMatchOneByte(state, node, pos)) {
                    matched = false;
                    break;
                }
                pos++;
                node = node->next;
            }
        } else {
            while (node != NULL &&
                   (node->kind == RX_CHAR || node->kind == RX_ANY || node->kind == RX_CLASS)) {
                if (!rxMatchOne(state, node, pos)) {
                    matched = false;
                    break;
                }
                pos++;
                node = node->next;
            }
        }
        result = matched && rxMatchNode(state, node, cont, pos);
        break;
    }

    case RX_BOL:
        if (pos == 0 || ((state->flags & BS_REGEX_MULTILINE) != 0 && rxCode(state, pos - 1) == '\n')) {
            result = rxMatchNode(state, node->next, cont, pos);
        }
        break;

    case RX_EOL:
        if (pos == state->length ||
            ((state->flags & BS_REGEX_MULTILINE) != 0 && rxCode(state, pos) == '\n')) {
            result = rxMatchNode(state, node->next, cont, pos);
        }
        break;

    case RX_WORD_BOUNDARY:
    case RX_NOT_WORD_BOUNDARY: {
        bool before = pos > 0 && rxIsWordCode(rxCode(state, pos - 1));
        bool after = pos < state->length && rxIsWordCode(rxCode(state, pos));
        bool boundary = (before != after);
        if (boundary == (node->kind == RX_WORD_BOUNDARY)) {
            result = rxMatchNode(state, node->next, cont, pos);
        }
        break;
    }

    case RX_ALT: {
        RxCont after = {RX_CONT_NODE, node->next, 0, 0, cont};
        for (size_t ix = 0; ix < node->u.alt.count && !result; ix++) {
            result = rxMatchNode(state, node->u.alt.branches[ix], &after, pos);
        }
        break;
    }

    case RX_GROUP: {
        size_t group = node->u.group.group;
        size_t mark = state->trailCount;
        rxTrailPush(state, group);
        state->match->groups[group].begin = pos;
        RxCont after = {RX_CONT_NODE, node->u.group.close, 0, 0, cont};
        result = rxMatchNode(state, node->u.group.sub, &after, pos);
        if (!result) {
            rxTrailUnwind(state, mark);
        }
        break;
    }

    case RX_GROUP_END: {
        size_t group = node->u.groupIndex;
        size_t mark = state->trailCount;
        rxTrailPush(state, group);
        state->match->groups[group].end = pos;
        state->match->matched[group] = true;
        result = rxMatchNode(state, node->next, cont, pos);
        if (!result) {
            rxTrailUnwind(state, mark);
        }
        break;
    }

    case RX_REPEAT:
        if (node->u.repeat.simple) {
            result = rxMatchSimpleRepeat(state, node, cont, pos);
        } else if (node->u.repeat.max == 0) {
            result = rxMatchNode(state, node->next, cont, pos);
        } else {
            RxCont iteration = {RX_CONT_REPEAT, node, 1, pos, cont};
            if (node->u.repeat.greedy || node->u.repeat.min > 0) {
                result = rxMatchNode(state, node->u.repeat.sub, &iteration, pos);
                if (!result && node->u.repeat.min == 0) {
                    result = rxMatchNode(state, node->next, cont, pos);
                }
            } else {
                result = rxMatchNode(state, node->next, cont, pos);
                if (!result) {
                    result = rxMatchNode(state, node->u.repeat.sub, &iteration, pos);
                }
            }
        }
        break;

    case RX_BACKREF: {
        size_t group = node->u.groupIndex;
        if (group >= state->regex->groupCount || !state->match->matched[group]) {
            /* An unmatched backreference matches the empty string */
            result = rxMatchNode(state, node->next, cont, pos);
            break;
        }
        BSRegexSpan span = state->match->groups[group];
        size_t size = span.end - span.begin;
        if (pos + size > state->length) {
            break;
        }
        bool equal = true;
        for (size_t ix = 0; ix < size && equal; ix++) {
            uint32_t expected = rxCode(state, span.begin + ix);
            uint32_t actual = rxCode(state, pos + ix);
            equal = (state->flags & BS_REGEX_IGNORECASE) != 0 ?
                rxFold(expected) == rxFold(actual) : expected == actual;
        }
        if (equal) {
            result = rxMatchNode(state, node->next, cont, pos + size);
        }
        break;
    }

    case RX_LOOKAHEAD: {
        const RxNode *atom = rxSimpleAtom(node->u.look.sub);
        if (atom != NULL) {
            bool matched = rxMatchOne(state, atom, pos);
            if (node->u.look.negate) {
                result = !matched && rxMatchNode(state, node->next, cont, pos);
            } else {
                result = matched && rxMatchNode(state, node->next, cont, pos);
            }
            break;
        }
        RxCont stop = {RX_CONT_STOP, NULL, 0, 0, NULL};
        size_t mark = state->trailCount;
        bool matched = rxMatchNode(state, node->u.look.sub, &stop, pos);
        if (node->u.look.negate) {
            rxTrailUnwind(state, mark);
            result = !matched && rxMatchNode(state, node->next, cont, pos);
        } else if (matched) {
            result = rxMatchNode(state, node->next, cont, pos);
            if (!result) {
                rxTrailUnwind(state, mark);
            }
        }
        break;
    }

    default: {
        /*
         * RX_LOOKBEHIND - try every start position whose distance from "pos" is a possible
         * sub-pattern match length, requiring the sub-pattern to end exactly at "pos"
         *
         * A lookbehind whose body is one code point - "(?<!\\)", "(?<=a)", "(?<![A-Za-z])" -
         * is a single membership test, the form markdown span matching uses on every candidate.
         */
        const RxNode *atom = (node->u.look.minLength == 1 && node->u.look.maxLength == 1) ?
            rxSimpleAtom(node->u.look.sub) : NULL;
        if (atom != NULL) {
            bool matched = pos >= 1 && rxMatchOne(state, atom, pos - 1);
            if (node->u.look.negate) {
                result = !matched && rxMatchNode(state, node->next, cont, pos);
            } else {
                result = matched && rxMatchNode(state, node->next, cont, pos);
            }
            break;
        }

        size_t mark = state->trailCount;
        size_t minLength = node->u.look.minLength;
        size_t maxLength = node->u.look.maxLength > pos ? pos : node->u.look.maxLength;
        bool matched = false;
        for (size_t length = minLength; length <= maxLength && !matched; length++) {
            RxCont anchor = {RX_CONT_ANCHOR, NULL, 0, pos, NULL};
            matched = rxMatchNode(state, node->u.look.sub, &anchor, pos - length);
            if (!matched) {
                rxTrailUnwind(state, mark);
            }
        }
        if (node->u.look.negate) {
            rxTrailUnwind(state, mark);
            result = !matched && rxMatchNode(state, node->next, cont, pos);
        } else if (matched) {
            result = rxMatchNode(state, node->next, cont, pos);
            if (!result) {
                rxTrailUnwind(state, mark);
            }
        }
        break;
    }
    }

    state->depth--;
    return result;
}


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
    if (length <= sizeof(subject->inline_) / sizeof(subject->inline_[0])) {
        subject->codes = subject->inline_;
    } else {
        subject->owned = bsAlloc(length * sizeof(uint32_t));
        subject->codes = subject->owned;
    }
    uint32_t *codes = subject->owned != NULL ? subject->owned : subject->inline_;
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
    state.regex = compiled;
    state.match = match;
    state.end = 0;
    state.depth = 0;
    state.steps = 0;
    state.trail = state.trailInline;
    state.trailCount = 0;
    state.trailCapacity = sizeof(state.trailInline) / sizeof(state.trailInline[0]);

    /* Only the pattern's own capture groups need clearing, not the whole capture array */
    size_t groupBytes = compiled->groupCount * sizeof(BSRegexSpan);
    size_t matchedBytes = compiled->groupCount * sizeof(bool);
    size_t last = compiled->anchored ? start : subject->length;
    for (size_t pos = start; pos <= last; pos++) {
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
        memset(match->groups, 0, groupBytes);
        memset(match->matched, 0, matchedBytes);
        match->begin = 0;
        match->end = 0;
        match->groupCount = compiled->groupCount;
        RxCont end = {RX_CONT_END, NULL, 0, 0, NULL};
        state.depth = 0;
        state.steps = 0;
        state.trailCount = 0;
        if (rxMatchNode(&state, compiled->root, &end, pos)) {
            match->begin = pos;
            match->end = state.end;
            match->groups[0].begin = pos;
            match->groups[0].end = state.end;
            match->matched[0] = true;
            if (state.trail != state.trailInline) {
                free(state.trail);
            }
            return true;
        }
    }
    if (state.trail != state.trailInline) {
        free(state.trail);
    }
    return false;
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
        if ((unsigned char) ch < 0x80 && strchr(special, ch) != NULL) {
            bsSBAppendChar(&sb, '\\');
        }
        bsSBAppendChar(&sb, ch);
    }
    return bsSBToValue(&sb);
}
