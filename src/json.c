/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * A targeted JSON encoder/decoder for BareScript
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "barescript/json.h"

#include "internal.h"


/* The maximum JSON nesting depth */
#define BS_JSON_DEPTH_MAX 1000


/*
 * Encode
 */


/*
 * The escape for a byte an encoded string escapes - the control characters, the quote, and the
 * backslash - or NULL for a byte that stands as it is
 */
static const char *bsJSONEscape(unsigned char ch)
{
    static const char *const controls[0x20] = {
        "\\u0000", "\\u0001", "\\u0002", "\\u0003", "\\u0004", "\\u0005", "\\u0006", "\\u0007",
        "\\b", "\\t", "\\n", "\\u000b", "\\f", "\\r", "\\u000e", "\\u000f",
        "\\u0010", "\\u0011", "\\u0012", "\\u0013", "\\u0014", "\\u0015", "\\u0016", "\\u0017",
        "\\u0018", "\\u0019", "\\u001a", "\\u001b", "\\u001c", "\\u001d", "\\u001e", "\\u001f"
    };
    return ch < 0x20 ? controls[ch] : ch == '"' ? "\\\"" : ch == '\\' ? "\\\\" : NULL;
}


static void bsJSONEncodeString(BSStringBuilder *sb, BSValue value)
{
    const char *data = bsStringSpan(value);
    size_t size = bsStringSize(value);
    bsSBAppendChar(sb, '"');
    size_t begin = 0;
    for (size_t ix = 0; ix < size; ix++) {
        const char *escape = bsJSONEscape((unsigned char) data[ix]);
        if (escape == NULL) {
            continue;
        }
        bsSBAppend(sb, data + begin, ix - begin);
        bsSBAppendString(sb, escape);
        begin = ix + 1;
    }
    bsSBAppend(sb, data + begin, size - begin);
    bsSBAppendChar(sb, '"');
}


static void bsJSONIndent(BSStringBuilder *sb, int indent, int depth)
{
    if (indent > 0) {
        static const char spaces[] = "                                                                ";
        const size_t spaceCount = sizeof(spaces) - 1;
        bsSBAppendChar(sb, '\n');
        size_t count = (size_t) indent * (size_t) depth;
        while (count > spaceCount) {
            bsSBAppend(sb, spaces, spaceCount);
            count -= spaceCount;
        }
        bsSBAppend(sb, spaces, count);
    }
}


static void bsJSONEncodeValue(BSStringBuilder *sb, BSValue value, int indent, int depth);


typedef struct BSJSONObjectContext {
    BSStringBuilder *sb;
    int indent;
    int depth;
    bool first;
} BSJSONObjectContext;


static bool bsJSONEncodeMember(BSValue key, BSValue item, void *data)
{
    BSJSONObjectContext *context = data;
    if (!context->first) {
        bsSBAppendChar(context->sb, ',');
    }
    context->first = false;
    bsJSONIndent(context->sb, context->indent, context->depth + 1);
    bsJSONEncodeString(context->sb, key);
    bsSBAppendChar(context->sb, ':');
    if (context->indent > 0) {
        bsSBAppendChar(context->sb, ' ');
    }
    bsJSONEncodeValue(context->sb, item, context->indent, context->depth + 1);
    return true;
}


static void bsJSONEncodeValue(BSStringBuilder *sb, BSValue value, int indent, int depth)
{
    /* Nesting deeper than the limit - or a value cycle - encodes as null */
    if (depth >= BS_JSON_DEPTH_MAX) {
        bsSBAppendString(sb, "null");
        return;
    }

    switch (value.type) {
    case BS_NULL:
        bsSBAppendString(sb, "null");
        break;

    case BS_BOOLEAN:
        bsSBAppendString(sb, value.u.boolean ? "true" : "false");
        break;

    case BS_NUMBER:
        if (!isfinite(value.u.number)) {
            bsSBAppendString(sb, "null");
        } else {
            bsSBAppendValue(sb, value);
        }
        break;

    case BS_STRING:
        bsJSONEncodeString(sb, value);
        break;

    case BS_DATETIME:
    case BS_FUNCTION: {
        BSValue text = bsValueString(value);
        bsJSONEncodeString(sb, text);
        bsRelease(text);
        break;
    }

    case BS_ARRAY: {
        size_t count = bsArrayCount(value);
        if (count == 0) {
            bsSBAppendString(sb, "[]");
            break;
        }
        bsSBAppendChar(sb, '[');
        for (size_t ix = 0; ix < count; ix++) {
            if (ix != 0) {
                bsSBAppendChar(sb, ',');
            }
            bsJSONIndent(sb, indent, depth + 1);
            bsJSONEncodeValue(sb, bsArrayGet(value, ix), indent, depth + 1);
        }
        bsJSONIndent(sb, indent, depth);
        bsSBAppendChar(sb, ']');
        break;
    }

    case BS_OBJECT: {
        if (bsObjectCount(value) == 0) {
            bsSBAppendString(sb, "{}");
            break;
        }
        bsSBAppendChar(sb, '{');
        BSJSONObjectContext context = {sb, indent, depth, true};
        bsObjectIterSorted(value, bsJSONEncodeMember, &context);
        bsJSONIndent(sb, indent, depth);
        bsSBAppendChar(sb, '}');
        break;
    }

    default:
        /* A regex value has no JSON representation */
        bsSBAppendString(sb, "null");
        break;
    }
}


BSValue bsJSONEncode(BSValue value, int indent)
{
    BSStringBuilder sb;
    bsSBInit(&sb);
    bsJSONEncodeValue(&sb, value, indent, 0);
    return bsSBToValue(&sb);
}


/*
 * Decode
 *
 * The decoder's errors - their text and the position they report - match CPython's json module,
 * which is what the Python implementation's jsonParse surfaces, so a BareScript program sees the
 * same diagnostic on every implementation.
 */


/*
 * The key memo: the keys of the last object decoded at each nesting depth, by position. Records
 * repeat a shape, so a key is usually the string decoded at the same position of the last
 * object at this depth - one compare instead of a hash and an intern table probe.
 */
#define BS_JSON_MEMO_DEPTH 6
#define BS_JSON_MEMO_KEYS 12

typedef struct BSJSONParser {
    const char *text;
    size_t size;
    size_t offset;
    const char *error;
    size_t errorOffset;
    BSValue *memo;     /* BS_JSON_MEMO_DEPTH x BS_JSON_MEMO_KEYS keys, allocated at the first key */
    int memoDepth;     /* the object being decoded: its depth, and the position of the key at hand */
    size_t memoIndex;
    bool memoHit;      /* the key at hand came from the memo */
    size_t memoCount[BS_JSON_MEMO_DEPTH];   /* the last object at each depth: its key count, and whether */
    bool memoDistinct[BS_JSON_MEMO_DEPTH];  /* every key was new to it - so a record repeating its keys in */
    size_t memoItems[BS_JSON_MEMO_DEPTH];   /* order has distinct keys too, and appends without a scan; and */
} BSJSONParser;                             /* the last array's item count, which sizes the next one */


/*
 * A container's capacity from the memo's count for its depth: records and the arrays in them repeat
 * a shape, so the next one is born at the last one's size and grows through nothing. Capped so a
 * large container does not size every small one that follows it.
 */
#define BS_JSON_PRESIZE_MAX 64

static size_t bsJSONPresize(const size_t *memo, int depth)
{
    size_t count = depth < BS_JSON_MEMO_DEPTH ? memo[depth] : 0;
    return count <= BS_JSON_PRESIZE_MAX ? count : 0;
}


/* Record a decoding error and its position. Always returns false, for the caller to return. */
static bool bsJSONError(BSJSONParser *parser, const char *error, size_t offset)
{
    parser->error = error;
    parser->errorOffset = offset;
    return false;
}


static bool bsJSONStringError(BSJSONParser *parser, BSStringBuilder *sb, const char *error, size_t offset)
{
    bsSBFree(sb);
    return bsJSONError(parser, error, offset);
}


static void bsJSONSkipSpace(BSJSONParser *parser)
{
    while (parser->offset < parser->size) {
        char ch = parser->text[parser->offset];
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
            break;
        }
        parser->offset++;
    }
}


/* Skip space and consume "ch" when it is next. True if it was taken. */
static bool bsJSONSkipTake(BSJSONParser *parser, char ch)
{
    bsJSONSkipSpace(parser);
    if (parser->offset < parser->size && parser->text[parser->offset] == ch) {
        parser->offset++;
        return true;
    }
    return false;
}


static bool bsJSONLiteral(BSJSONParser *parser, const char *literal)
{
    size_t size = strlen(literal);
    if (parser->size - parser->offset < size || memcmp(parser->text + parser->offset, literal, size) != 0) {
        return false;
    }
    parser->offset += size;
    return true;
}


static bool bsJSONHex4(BSJSONParser *parser, uint32_t *result)
{
    if (parser->size - parser->offset < 4) {
        return false;
    }
    uint32_t value = 0;
    for (size_t ix = 0; ix < 4; ix++) {
        int digit = bsHexValue(parser->text[parser->offset + ix]);
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | (uint32_t) digit;
    }
    parser->offset += 4;
    *result = value;
    return true;
}


/* The character each single-character escape stands for */
static const char bsJSONUnescape[256] = {
    ['"'] = '"', ['\\'] = '\\', ['/'] = '/', ['b'] = '\b', ['f'] = '\f', ['n'] = '\n', ['r'] = '\r', ['t'] = '\t'
};


/* A key: the memo's string for this depth and position when it is the same, else an interned name when there is one */
static BSValue bsJSONKey(BSJSONParser *parser, const char *data, size_t size)
{
    parser->memoHit = false;
    if (parser->memoDepth >= BS_JSON_MEMO_DEPTH || parser->memoIndex >= BS_JSON_MEMO_KEYS) {
        return bsStringInternExisting(data, size);
    }
    if (parser->memo == NULL) {
        parser->memo = bsAlloc(BS_JSON_MEMO_DEPTH * BS_JSON_MEMO_KEYS * sizeof(BSValue));
        for (size_t ix = 0; ix < BS_JSON_MEMO_DEPTH * BS_JSON_MEMO_KEYS; ix++) {
            parser->memo[ix] = bsNull();
        }
    }
    BSValue *slot = &parser->memo[parser->memoDepth * BS_JSON_MEMO_KEYS + parser->memoIndex];
    if (slot->type == BS_STRING && slot->u.string->size == size && memcmp(slot->u.string->data, data, size) == 0) {
        parser->memoHit = true;
        return bsRetain(*slot);
    }
    BSValue key = bsStringInternExisting(data, size);
    bsAssign(slot, bsRetain(key));
    return key;
}

/* A decoded string: a plain string, or a key */
static BSValue bsJSONString(BSJSONParser *parser, const char *data, size_t size, bool key)
{
    return key ? bsJSONKey(parser, data, size) : bsStringNewSize(data, size);
}

/*
 * Decode the string at the offset, which holds its opening quote, when it holds no escape - the
 * common case, copied once from the input. False for a string with an escape or an error, which
 * bsJSONDecodeString then decodes through a builder.
 */
static inline bool bsJSONDecodePlainString(BSJSONParser *parser, BSValue *result, bool key)
{
    size_t begin = parser->offset + 1;
    size_t ix = begin;
    while (ix < parser->size) {
        unsigned char ch = (unsigned char) parser->text[ix];
        if (ch == '"') {
            *result = bsJSONString(parser, parser->text + begin, ix - begin, key);
            parser->offset = ix + 1;
            return true;
        }
        if (ch == '\\' || ch < 0x20) {
            break;
        }
        ix++;
    }
    return false;
}


/* Decode the string at the offset, which holds its opening quote, once the plain decode has declined it */
static BS_NOINLINE bool bsJSONDecodeString(BSJSONParser *parser, BSValue *result, bool key)
{
    size_t begin = parser->offset++;
    BSStringBuilder sb;
    bsSBInit(&sb);
    char utf8[4];
    while (true) {
        if (parser->offset >= parser->size) {
            return bsJSONStringError(parser, &sb, "Unterminated string starting at", begin);
        }
        char ch = parser->text[parser->offset];
        if (ch == '"') {
            parser->offset++;
            break;
        }
        if (ch != '\\') {
            if ((unsigned char) ch < 0x20) {
                return bsJSONStringError(parser, &sb, "Invalid control character at", parser->offset);
            }
            bsSBAppendChar(&sb, ch);
            parser->offset++;
            continue;
        }

        size_t escapeOffset = parser->offset;
        parser->offset++;
        if (parser->offset >= parser->size) {
            return bsJSONStringError(parser, &sb, "Unterminated string starting at", begin);
        }
        char escape = parser->text[parser->offset++];
        if (bsJSONUnescape[(unsigned char) escape] != '\0') {
            bsSBAppendChar(&sb, bsJSONUnescape[(unsigned char) escape]);
        } else if (escape == 'u') {
            uint32_t codePoint;
            if (!bsJSONHex4(parser, &codePoint)) {
                return bsJSONStringError(parser, &sb, "Invalid \\uXXXX escape", escapeOffset + 1);
            }

            /* Combine a surrogate pair */
            if (codePoint >= 0xD800 && codePoint <= 0xDBFF && parser->size - parser->offset >= 6 &&
                parser->text[parser->offset] == '\\' && parser->text[parser->offset + 1] == 'u') {
                size_t save = parser->offset;
                parser->offset += 2;
                uint32_t low;
                if (bsJSONHex4(parser, &low) && low >= 0xDC00 && low <= 0xDFFF) {
                    codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                } else {
                    parser->offset = save;
                }
            }

            /* An unpaired surrogate has no UTF-8 encoding - use the replacement character */
            if (codePoint >= 0xD800 && codePoint <= 0xDFFF) {
                codePoint = 0xFFFD;
            }
            bsSBAppend(&sb, utf8, bsUTF8Encode(codePoint, utf8));
        } else {
            return bsJSONStringError(parser, &sb, "Invalid \\escape", escapeOffset);
        }
    }

    *result = bsJSONString(parser, sb.data, sb.size, key);
    bsSBFree(&sb);
    return true;
}


static bool bsJSONDecodeQuoted(BSJSONParser *parser, BSValue *result, bool key)
{
    return bsJSONDecodePlainString(parser, result, key) || bsJSONDecodeString(parser, result, key);
}


static bool bsJSONDecodeValue(BSJSONParser *parser, int depth, BSValue *result);


/*
 * After a container item: the container's closing character ends it (1), a comma continues it
 * (0), and anything else - or a comma before the close - is an error, set on the parser (-1)
 */
static int bsJSONSeparator(BSJSONParser *parser, char close)
{
    if (bsJSONSkipTake(parser, close)) {
        return 1;
    }
    if (parser->offset >= parser->size || parser->text[parser->offset] != ',') {
        bsJSONError(parser, "Expecting ',' delimiter", parser->offset);
        return -1;
    }
    size_t commaOffset = parser->offset++;
    if (bsJSONSkipTake(parser, close)) {
        bsJSONError(parser, close == ']' ? "Illegal trailing comma before end of array" :
                    "Illegal trailing comma before end of object", commaOffset);
        return -1;
    }
    return 0;
}


static bool bsJSONDecodeArray(BSJSONParser *parser, int depth, BSValue *result)
{
    parser->offset++;
    BSValue array = bsArrayNewCapacity(bsJSONPresize(parser->memoItems, depth));
    if (bsJSONSkipTake(parser, ']')) {
        if (depth < BS_JSON_MEMO_DEPTH) {
            parser->memoItems[depth] = 0;
        }
        *result = array;
        return true;
    }
    while (true) {
        BSValue item;
        if (!bsJSONDecodeValue(parser, depth + 1, &item)) {
            bsRelease(array);
            return false;
        }
        bsArrayPush(array, item);
        int separator = bsJSONSeparator(parser, ']');
        if (separator < 0) {
            bsRelease(array);
            return false;
        }
        if (separator > 0) {
            break;
        }
    }
    if (depth < BS_JSON_MEMO_DEPTH) {
        parser->memoItems[depth] = bsArrayCount(array);
    }
    *result = array;
    return true;
}


/* Decode an object member's key and the colon after it. Returns an owned key. */
static bool bsJSONDecodeKey(BSJSONParser *parser, BSValue *key)
{
    bsJSONSkipSpace(parser);
    if (parser->offset >= parser->size || parser->text[parser->offset] != '"') {
        return bsJSONError(parser, "Expecting property name enclosed in double quotes", parser->offset);
    }
    if (!bsJSONDecodeQuoted(parser, key, true)) {
        return false;
    }
    if (!bsJSONSkipTake(parser, ':')) {
        bsRelease(*key);
        return bsJSONError(parser, "Expecting ':' delimiter", parser->offset);
    }
    return true;
}


static bool bsJSONDecodeObject(BSJSONParser *parser, int depth, BSValue *result)
{
    parser->offset++;
    BSValue object = bsObjectNewSized(bsJSONPresize(parser->memoCount, depth), false);
    if (bsJSONSkipTake(parser, '}')) {
        if (depth < BS_JSON_MEMO_DEPTH) {
            parser->memoCount[depth] = 0;
        }
        *result = object;
        return true;
    }

    /*
     * A key that repeats the last object's at its position, every key so far having done so and
     * that object's keys having all been new to it, is new to this object too: appended, no scan
     */
    bool memoed = depth < BS_JSON_MEMO_DEPTH;
    bool inOrder = memoed && parser->memoDistinct[depth];
    size_t previousCount = memoed ? parser->memoCount[depth] : 0;
    bool distinct = true;
    size_t index = 0;
    for (;; index++) {
        BSValue key;
        parser->memoDepth = depth;
        parser->memoIndex = index;
        if (!bsJSONDecodeKey(parser, &key)) {
            bsRelease(object);
            return false;
        }
        BSValue item;
        if (!bsJSONDecodeValue(parser, depth + 1, &item)) {
            bsRelease(key);
            bsRelease(object);
            return false;
        }
        inOrder = inOrder && parser->memoHit && index < previousCount;
        if (inOrder) {
            bsObjectAppend(object, key, item);
        } else {
            size_t count = bsObjectCount(object);
            bsObjectSetString(object, key, item);
            distinct = distinct && bsObjectCount(object) > count;
        }
        bsRelease(key);
        int separator = bsJSONSeparator(parser, '}');
        if (separator < 0) {
            bsRelease(object);
            return false;
        }
        if (separator > 0) {
            break;
        }
    }
    if (memoed) {
        parser->memoCount[depth] = index + 1;
        parser->memoDistinct[depth] = distinct;
    }
    *result = object;
    return true;
}


/*
 * Decode a number
 *
 * The grammar is CPython's NUMBER_RE - "-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][-+]?[0-9]+)?" - which
 * rejects a leading zero and simply stops at anything it cannot consume, so "01" and "1." are a
 * number followed by trailing text rather than a malformed number.
 */
static bool bsJSONDecodeNumber(BSJSONParser *parser, BSValue *result)
{
    const char *text = parser->text;
    size_t size = parser->size;
    size_t begin = parser->offset;
    size_t ix = begin;

    if (ix < size && text[ix] == '-') {
        ix++;
    }
    if (ix >= size) {
        return bsJSONError(parser, "Expecting value", begin);
    }
    if (text[ix] == '0') {
        ix++;
    } else if (text[ix] >= '1' && text[ix] <= '9') {
        ix = bsSkipDigits(text, size, ix);
    } else {
        return bsJSONError(parser, "Expecting value", begin);
    }
    if (ix + 1 < size && text[ix] == '.' && text[ix + 1] >= '0' && text[ix + 1] <= '9') {
        ix = bsSkipDigits(text, size, ix + 2);
    }
    if (ix < size && (text[ix] == 'e' || text[ix] == 'E')) {
        size_t save = ix;
        ix++;
        if (ix < size && (text[ix] == '-' || text[ix] == '+')) {
            ix++;
        }
        if (ix < size && text[ix] >= '0' && text[ix] <= '9') {
            ix = bsSkipDigits(text, size, ix);
        } else {
            ix = save;
        }
    }

    /* A number past the double range is null, the value the encoder writes for a non-finite number */
    double value = bsStrtod(text + begin, ix - begin);
    *result = isfinite(value) ? bsNumber(value) : bsNull();
    parser->offset = ix;
    return true;
}


static bool bsJSONDecodeValue(BSJSONParser *parser, int depth, BSValue *result)
{
    if (depth >= BS_JSON_DEPTH_MAX) {
        return bsJSONError(parser, "Maximum nesting depth exceeded", parser->offset);
    }
    bsJSONSkipSpace(parser);
    if (parser->offset >= parser->size) {
        return bsJSONError(parser, "Expecting value", parser->offset);
    }
    char ch = parser->text[parser->offset];
    if (ch == '{') {
        return bsJSONDecodeObject(parser, depth, result);
    }
    if (ch == '[') {
        return bsJSONDecodeArray(parser, depth, result);
    }
    if (ch == '"') {
        return bsJSONDecodeQuoted(parser, result, false);
    }
    if (ch == 't' && bsJSONLiteral(parser, "true")) {
        *result = bsBoolean(true);
        return true;
    }
    if (ch == 'f' && bsJSONLiteral(parser, "false")) {
        *result = bsBoolean(false);
        return true;
    }
    if (ch == 'n' && bsJSONLiteral(parser, "null")) {
        *result = bsNull();
        return true;
    }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        return bsJSONDecodeNumber(parser, result);
    }
    return bsJSONError(parser, "Expecting value", parser->offset);
}


/*
 * Intern the closed set of script-model object keys before decoding JSON, so JSON insert reuses
 * interned names and later interned lookup stays pointer-only. Unique payload keys stay ordinary
 * strings and do not grow the intern table.
 */
BSValue bsJSONDecodeEx(const char *text, size_t size, const char **error, size_t *errorOffset)
{
    bsModelKeysInit();
    BSJSONParser parser = {.text = text, .size = size};
    BSValue result;
    bool decoded = bsJSONDecodeValue(&parser, 0, &result);
    if (parser.memo != NULL) {
        for (size_t ix = 0; ix < BS_JSON_MEMO_DEPTH * BS_JSON_MEMO_KEYS; ix++) {
            bsRelease(parser.memo[ix]);
        }
        free(parser.memo);
    }
    if (decoded) {
        bsJSONSkipSpace(&parser);
        if (parser.offset != parser.size) {
            bsRelease(result);
            decoded = bsJSONError(&parser, "Extra data", parser.offset);
        }
    }
    if (error != NULL) {
        *error = decoded ? NULL : parser.error;
    }
    if (errorOffset != NULL && !decoded) {
        *errorOffset = parser.errorOffset;
    }
    return decoded ? result : bsNull();
}


BSValue bsJSONDecode(const char *text, size_t size, const char **error)
{
    return bsJSONDecodeEx(text, size, error, NULL);
}

