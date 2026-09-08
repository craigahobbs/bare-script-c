/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * A targeted JSON encoder/decoder for BareScript
 */

#include <math.h>
#include <string.h>

#include "barescript/json.h"

#include "internal.h"


/* The maximum JSON nesting depth */
#define BS_JSON_DEPTH_MAX 1000


/*
 * Encode
 */


/* The escape for a byte an encoded string escapes - the control characters, the quote, and the
 * backslash - or NULL for a byte that stands as it is */
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
    const char *data = bsStringData(value);
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


typedef struct BSJSONParser {
    const char *text;
    size_t size;
    size_t offset;
    const char *error;
    size_t errorOffset;
} BSJSONParser;


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


/* How a decoded string is made: a plain string, a key (an interned name when there is one), or an interned string */
enum {
    BS_JSON_STRING_PLAIN,
    BS_JSON_STRING_KEY,
    BS_JSON_STRING_INTERN
};

static BSValue bsJSONString(const char *data, size_t size, int mode)
{
    return mode == BS_JSON_STRING_PLAIN ? bsStringNewSize(data, size) :
        mode == BS_JSON_STRING_KEY ? bsStringInternExisting(data, size) : bsStringIntern(data, size);
}

/*
 * Decode the string at the offset, which holds its opening quote, when it holds no escape - the
 * common case, copied once from the input. False for a string with an escape or an error, which
 * bsJSONDecodeString then decodes through a builder.
 */
static inline bool bsJSONDecodePlainString(BSJSONParser *parser, BSValue *result, int mode)
{
    size_t begin = parser->offset + 1;
    size_t ix = begin;
    while (ix < parser->size) {
        unsigned char ch = (unsigned char) parser->text[ix];
        if (ch == '"') {
            *result = bsJSONString(parser->text + begin, ix - begin, mode);
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


/* Decode the string at the offset, which holds its opening quote */
static BS_NOINLINE bool bsJSONDecodeString(BSJSONParser *parser, BSValue *result, int mode)
{
    if (bsJSONDecodePlainString(parser, result, mode)) {
        return true;
    }
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

    *result = bsJSONString(sb.data, sb.size, mode);
    bsSBFree(&sb);
    return true;
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
    BSValue array = bsArrayNew();
    if (bsJSONSkipTake(parser, ']')) {
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
    if (!bsJSONDecodePlainString(parser, key, BS_JSON_STRING_KEY) && !bsJSONDecodeString(parser, key, BS_JSON_STRING_KEY)) {
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
    BSValue object = bsObjectNew();
    if (bsJSONSkipTake(parser, '}')) {
        *result = object;
        return true;
    }
    while (true) {
        BSValue key;
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
        bsObjectSetString(object, key, item);
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
    *result = object;
    return true;
}


static void bsJSONScanDigits(const char *text, size_t size, size_t *ix)
{
    while (*ix < size && text[*ix] >= '0' && text[*ix] <= '9') {
        (*ix)++;
    }
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
        bsJSONScanDigits(text, size, &ix);
    } else {
        return bsJSONError(parser, "Expecting value", begin);
    }
    if (ix + 1 < size && text[ix] == '.' && text[ix + 1] >= '0' && text[ix + 1] <= '9') {
        ix += 2;
        bsJSONScanDigits(text, size, &ix);
    }
    if (ix < size && (text[ix] == 'e' || text[ix] == 'E')) {
        size_t save = ix;
        ix++;
        if (ix < size && (text[ix] == '-' || text[ix] == '+')) {
            ix++;
        }
        if (ix < size && text[ix] >= '0' && text[ix] <= '9') {
            bsJSONScanDigits(text, size, &ix);
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
    size_t begin = parser->offset;
    char ch = parser->text[begin];
    if (ch == '{') {
        return bsJSONDecodeObject(parser, depth, result);
    }
    if (ch == '[') {
        return bsJSONDecodeArray(parser, depth, result);
    }
    if (ch == '"') {
        return bsJSONDecodePlainString(parser, result, BS_JSON_STRING_PLAIN) ||
            bsJSONDecodeString(parser, result, BS_JSON_STRING_PLAIN);
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
    return bsJSONError(parser, "Expecting value", begin);
}


/*
 * Intern the closed set of script-model object keys before decoding JSON, so JSON insert reuses
 * interned names and later interned lookup stays pointer-only. Unique payload keys stay ordinary
 * strings and do not grow the intern table.
 */
BSValue bsJSONDecodeEx(const char *text, size_t size, const char **error, size_t *errorOffset)
{
    bsModelKeysInit();
    BSJSONParser parser = {text, size, 0, NULL, 0};
    BSValue result;
    bool decoded = bsJSONDecodeValue(&parser, 0, &result);
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


/*
 * The model reader
 *
 * A script model's JSON is read straight into the emitter's syntax tree, a statement at a time,
 * with a node's kind and members recognized by their key bytes, so no model object is built. The
 * grammar is the decoder's own: a value whose shape the model does not allow is decoded again as
 * a plain value, so a syntax error inside it reports as one and a well-formed one as an invalid
 * model, just as it would were the whole statement decoded first.
 */


/* The model keys the reader knows, by their bytes */
typedef enum {
    BS_MKEY_OTHER,
    BS_MKEY_ARGS, BS_MKEY_BINARY, BS_MKEY_EXPR, BS_MKEY_FUNCTION, BS_MKEY_GROUP, BS_MKEY_INCLUDE,
    BS_MKEY_INCLUDES, BS_MKEY_JUMP, BS_MKEY_LABEL, BS_MKEY_LAST_ARG_ARRAY, BS_MKEY_LEFT,
    BS_MKEY_LINE_NUMBER, BS_MKEY_NAME, BS_MKEY_NUMBER, BS_MKEY_OP, BS_MKEY_RETURN, BS_MKEY_RIGHT,
    BS_MKEY_STATEMENTS, BS_MKEY_STRING, BS_MKEY_SYSTEM, BS_MKEY_UNARY, BS_MKEY_URL, BS_MKEY_VARIABLE
} BSModelKey;


static BS_NOINLINE BSModelKey bsModelKeyOf(const char *data, size_t size)
{
    /* The keys are told apart by size and first character - "string" and "system" by the second */
    const char *text = NULL;
    BSModelKey key = BS_MKEY_OTHER;
    switch (size) {
    case 2:
        text = "op";
        key = BS_MKEY_OP;
        break;
    case 3:
        text = "url";
        key = BS_MKEY_URL;
        break;
    case 4:
        switch (data[0]) {
        case 'a':
            text = "args";
            key = BS_MKEY_ARGS;
            break;
        case 'e':
            text = "expr";
            key = BS_MKEY_EXPR;
            break;
        case 'j':
            text = "jump";
            key = BS_MKEY_JUMP;
            break;
        case 'l':
            text = "left";
            key = BS_MKEY_LEFT;
            break;
        case 'n':
            text = "name";
            key = BS_MKEY_NAME;
            break;
        default:
            break;
        }
        break;
    case 5:
        switch (data[0]) {
        case 'g':
            text = "group";
            key = BS_MKEY_GROUP;
            break;
        case 'l':
            text = "label";
            key = BS_MKEY_LABEL;
            break;
        case 'r':
            text = "right";
            key = BS_MKEY_RIGHT;
            break;
        case 'u':
            text = "unary";
            key = BS_MKEY_UNARY;
            break;
        default:
            break;
        }
        break;
    case 6:
        switch (data[0]) {
        case 'b':
            text = "binary";
            key = BS_MKEY_BINARY;
            break;
        case 'n':
            text = "number";
            key = BS_MKEY_NUMBER;
            break;
        case 'r':
            text = "return";
            key = BS_MKEY_RETURN;
            break;
        case 's':
            text = data[1] == 't' ? "string" : "system";
            key = data[1] == 't' ? BS_MKEY_STRING : BS_MKEY_SYSTEM;
            break;
        default:
            break;
        }
        break;
    case 7:
        text = "include";
        key = BS_MKEY_INCLUDE;
        break;
    case 8:
        switch (data[0]) {
        case 'f':
            text = "function";
            key = BS_MKEY_FUNCTION;
            break;
        case 'i':
            text = "includes";
            key = BS_MKEY_INCLUDES;
            break;
        case 'v':
            text = "variable";
            key = BS_MKEY_VARIABLE;
            break;
        default:
            break;
        }
        break;
    case 10:
        text = data[0] == 'l' ? "lineNumber" : "statements";
        key = data[0] == 'l' ? BS_MKEY_LINE_NUMBER : BS_MKEY_STATEMENTS;
        break;
    case 12:
        text = "lastArgArray";
        key = BS_MKEY_LAST_ARG_ARRAY;
        break;
    default:
        break;
    }
    return text != NULL && memcmp(data, text, size) == 0 ? key : BS_MKEY_OTHER;
}


/*
 * Read an object member's key and the colon after it. The key's bytes are in the text when it
 * has no escape, and decoded into "*decoded" - which the caller releases - otherwise.
 */
static BS_NOINLINE bool bsJSONReadKey(BSJSONParser *parser, const char **data, size_t *size, BSValue *decoded)
{
    bsJSONSkipSpace(parser);
    if (parser->offset >= parser->size || parser->text[parser->offset] != '"') {
        return bsJSONError(parser, "Expecting property name enclosed in double quotes", parser->offset);
    }
    size_t begin = parser->offset + 1;
    size_t ix = begin;
    while (ix < parser->size && parser->text[ix] != '"' && parser->text[ix] != '\\' &&
           (unsigned char) parser->text[ix] >= 0x20) {
        ix++;
    }
    *decoded = bsNull();
    if (ix < parser->size && parser->text[ix] == '"') {
        *data = parser->text + begin;
        *size = ix - begin;
        parser->offset = ix + 1;
    } else {
        if (!bsJSONDecodeString(parser, decoded, BS_JSON_STRING_KEY)) {
            return false;
        }
        *data = bsStringData(*decoded);
        *size = bsStringSize(*decoded);
    }
    if (!bsJSONSkipTake(parser, ':')) {
        bsRelease(*decoded);
        *decoded = bsNull();
        return bsJSONError(parser, "Expecting ':' delimiter", parser->offset);
    }
    return true;
}


/* Decode the value at the offset as any value, for its syntax alone */
static BS_NOINLINE bool bsJSONSkipValue(BSJSONParser *parser, int depth)
{
    BSValue value;
    if (!bsJSONDecodeValue(parser, depth, &value)) {
        return false;
    }
    bsRelease(value);
    return true;
}


/*
 * The reader's member and element callbacks stay out of line: called through pointers, the
 * profile-guided build would otherwise inline the hot ones into the loops, kilobytes at a time
 */

/* Whether the next character - past any space - is "ch" */
static bool bsJSONPeek(BSJSONParser *parser, char ch)
{
    bsJSONSkipSpace(parser);
    return parser->offset < parser->size && parser->text[parser->offset] == ch;
}


/*
 * Read a member's string value as an interned string in the arena, into "*text"; false for a
 * value of another shape, or a syntax error, which sets the parser's error
 */
static bool bsJSONReadText(BSJSONParser *parser, BSAst *ast, uint32_t *text)
{
    BSValue string;
    if (!bsJSONPeek(parser, '"') || !bsJSONDecodeString(parser, &string, BS_JSON_STRING_INTERN)) {
        return false;
    }
    *text = bsAstString(ast, string);
    return true;
}


/*
 * The members of the object at the offset, each handed to "member" with the parser at its value.
 * False on a syntax error - the parser's error is set - or when "member" returns false.
 */
typedef bool (*BSJSONMemberFn)(BSJSONParser *parser, BSModelKey key, int depth, void *context);

static BS_NOINLINE bool bsJSONReadMembers(BSJSONParser *parser, int depth, BSJSONMemberFn member, void *context)
{
    if (depth >= BS_JSON_DEPTH_MAX) {
        return bsJSONError(parser, "Maximum nesting depth exceeded", parser->offset);
    }
    if (!bsJSONSkipTake(parser, '{')) {
        return false;
    }
    if (bsJSONSkipTake(parser, '}')) {
        return true;
    }
    while (true) {
        const char *data;
        size_t size;
        BSValue decoded;
        if (!bsJSONReadKey(parser, &data, &size, &decoded)) {
            return false;
        }
        BSModelKey key = bsModelKeyOf(data, size);
        bsRelease(decoded);
        if (!member(parser, key, depth + 1, context)) {
            return false;
        }
        int separator = bsJSONSeparator(parser, '}');
        if (separator < 0) {
            return false;
        }
        if (separator > 0) {
            return true;
        }
    }
}


static uint32_t bsJSONReadExpr(BSJSONParser *parser, BSAst *ast, int depth);
static uint32_t bsJSONReadStatement(BSJSONParser *parser, BSAst *ast, int depth);


/* A node under construction: the arena, the node, and the list it is collecting - its "a" or "b" */
typedef struct BSJSONNode {
    BSAst *ast;
    uint32_t node;
    bool second;       /* the list is the node's "b" */
    uint32_t tail;
    uint32_t count;    /* the elements read, the malformed ones included */
    uint32_t failedAt; /* the index of the first malformed element, when "failed" */
    bool failed;
} BSJSONNode;


/* The first node of the list "context" collects */
static uint32_t *bsJSONNodeHead(BSJSONNode *context)
{
    BSNode *node = &context->ast->nodes[context->node];
    return context->second ? &node->b : &node->a;
}


/* Append a node to the list of "context" */
static void bsJSONNodeAppend(BSJSONNode *context, uint32_t node)
{
    if (context->tail == 0) {
        *bsJSONNodeHead(context) = node;
    } else {
        context->ast->nodes[context->tail].next = node;
    }
    context->tail = node;
    context->count++;
}


/* Read a node - one of "kind", with its members read by "member" into "context" - into the arena, or zero */
static uint32_t bsJSONReadNode(BSJSONParser *parser, int depth, uint8_t kind, BSJSONMemberFn member, BSJSONNode *context)
{
    context->node = bsAstNode(context->ast, kind);
    return bsJSONReadMembers(parser, depth, member, context) ? context->node : 0;
}


/* Read an expression into a node's "a" - or "b" when "second" - or fail as bsJSONReadExpr does */
static bool bsJSONReadExprInto(BSJSONParser *parser, BSAst *ast, int depth, uint32_t node, bool second)
{
    uint32_t expr = bsJSONReadExpr(parser, ast, depth);
    if (expr == 0) {
        return false;
    }
    if (second) {
        ast->nodes[node].b = expr;
    } else {
        ast->nodes[node].a = expr;
    }
    return true;
}


/* Read an operator member into a node's op - a binary node's or a unary node's - or fail for another shape */
static bool bsJSONReadOperator(BSJSONParser *parser, BSAst *ast, uint32_t node, bool binary)
{
    BSValue op;
    if (!bsJSONPeek(parser, '"') || !bsJSONDecodeString(parser, &op, BS_JSON_STRING_PLAIN)) {
        return false;
    }
    uint16_t nodeOp = binary ? bsBinaryNodeOp(bsStringData(op)) : bsUnaryOpcode(bsStringData(op));
    bsRelease(op);
    ast->nodes[node].op = nodeOp;
    return nodeOp != 0;
}


/*
 * The elements of the array at the offset, each read by "element" - which returns false for a
 * syntax error, or for an element of another shape, which then decodes as any value with the
 * shape noted - into the list of "context"
 */
static BS_NOINLINE bool bsJSONReadElements(BSJSONParser *parser, int depth,
                                           bool (*element)(BSJSONParser *, int, BSJSONNode *), BSJSONNode *context)
{
    if (depth >= BS_JSON_DEPTH_MAX) {
        return bsJSONError(parser, "Maximum nesting depth exceeded", parser->offset);
    }
    parser->offset++;
    context->tail = 0;
    context->count = 0;
    *bsJSONNodeHead(context) = 0;
    if (bsJSONSkipTake(parser, ']')) {
        return true;
    }
    while (true) {
        bsJSONSkipSpace(parser);
        size_t begin = parser->offset;
        if (!element(parser, depth + 1, context)) {
            if (parser->error != NULL) {
                return false;
            }
            parser->offset = begin;
            if (!bsJSONSkipValue(parser, depth + 1)) {
                return false;
            }
            if (!context->failed) {
                context->failed = true;
                context->failedAt = context->count;
            }
            context->count++;
        }
        int separator = bsJSONSeparator(parser, ']');
        if (separator < 0) {
            return false;
        }
        if (separator > 0) {
            return true;
        }
    }
}


static BS_NOINLINE bool bsJSONReadArgElement(BSJSONParser *parser, int depth, BSJSONNode *context)
{
    uint32_t arg = bsJSONReadExpr(parser, context->ast, depth);
    if (arg == 0) {
        return false;
    }
    /* The conditional reads its first three arguments and no more, so a fourth of any shape stands */
    if (context->failed) {
        context->count++;
    } else {
        bsJSONNodeAppend(context, arg);
    }
    return true;
}


static BS_NOINLINE bool bsJSONCallMember(BSJSONParser *parser, BSModelKey key, int depth, void *data)
{
    BSJSONNode *context = data;
    BSAst *ast = context->ast;
    switch (key) {
    case BS_MKEY_ARGS:
        if (bsJSONPeek(parser, '[')) {
            return bsJSONReadElements(parser, depth, bsJSONReadArgElement, context);
        }
        return bsJSONSkipValue(parser, depth);
    case BS_MKEY_NAME:
        return bsJSONReadText(parser, ast, &ast->nodes[context->node].text);
    default:
        return bsJSONSkipValue(parser, depth);
    }
}


static BS_NOINLINE bool bsJSONBinaryMember(BSJSONParser *parser, BSModelKey key, int depth, void *data)
{
    BSJSONNode *context = data;
    switch (key) {
    case BS_MKEY_LEFT:
    case BS_MKEY_RIGHT:
        return bsJSONReadExprInto(parser, context->ast, depth, context->node, key == BS_MKEY_RIGHT);
    case BS_MKEY_OP:
        return bsJSONReadOperator(parser, context->ast, context->node, true);
    default:
        return bsJSONSkipValue(parser, depth);
    }
}


static BS_NOINLINE bool bsJSONUnaryMember(BSJSONParser *parser, BSModelKey key, int depth, void *data)
{
    BSJSONNode *context = data;
    switch (key) {
    case BS_MKEY_EXPR:
        return bsJSONReadExprInto(parser, context->ast, depth, context->node, false);
    case BS_MKEY_OP:
        return bsJSONReadOperator(parser, context->ast, context->node, false);
    default:
        return bsJSONSkipValue(parser, depth);
    }
}


/*
 * Read an expression - an object of one member whose key is its kind - into the arena. Returns
 * the node, or zero: with the parser's error set for a syntax error, and without one for a value
 * of another shape, which the caller then decodes as any value.
 */
static uint32_t bsJSONReadExpr(BSJSONParser *parser, BSAst *ast, int depth)
{
    if (depth >= BS_JSON_DEPTH_MAX) {
        bsJSONError(parser, "Maximum nesting depth exceeded", parser->offset);
        return 0;
    }
    if (!bsJSONSkipTake(parser, '{')) {
        return 0;
    }
    const char *data;
    size_t size;
    BSValue decoded;
    if (!bsJSONReadKey(parser, &data, &size, &decoded)) {
        return 0;
    }
    BSModelKey key = bsModelKeyOf(data, size);
    bsRelease(decoded);

    uint32_t node = 0;
    switch (key) {
    case BS_MKEY_NUMBER: {
        BSValue number;
        if (!bsJSONDecodeValue(parser, depth + 1, &number)) {
            return 0;
        }
        if (number.type != BS_NUMBER) {
            bsRelease(number);
            return 0;
        }
        node = bsAstNode(ast, BS_NODE_NUMBER);
        ast->nodes[node].number = number.u.number;
        break;
    }
    case BS_MKEY_STRING:
    case BS_MKEY_VARIABLE: {
        uint32_t text;
        if (!bsJSONReadText(parser, ast, &text)) {
            return 0;
        }
        node = bsAstNode(ast, key == BS_MKEY_STRING ? BS_NODE_STRING : BS_NODE_VARIABLE);
        ast->nodes[node].text = text;
        break;
    }
    case BS_MKEY_GROUP: {
        uint32_t sub = bsJSONReadExpr(parser, ast, depth + 1);
        if (sub == 0) {
            return 0;
        }
        node = bsAstNode(ast, BS_NODE_GROUP);
        ast->nodes[node].a = sub;
        break;
    }
    case BS_MKEY_FUNCTION: {
        BSJSONNode context = {.ast = ast};
        node = bsJSONReadNode(parser, depth + 1, BS_NODE_CALL, bsJSONCallMember, &context);
        if (node == 0 || ast->nodes[node].text == 0) {
            return 0;
        }
        /* A malformed argument past the three the conditional reads is not one it can see */
        bool isIf = bsNameIs(bsStringData(ast->strings[ast->nodes[node].text - 1]), "if");
        if (context.failed && !(isIf && context.failedAt >= 3)) {
            return 0;
        }
        ast->nodes[node].b = isIf && context.count > 3 ? 3 : context.count;
        break;
    }
    case BS_MKEY_BINARY:
    case BS_MKEY_UNARY: {
        bool binary = key == BS_MKEY_BINARY;
        BSJSONNode context = {.ast = ast};
        node = bsJSONReadNode(parser, depth + 1, binary ? BS_NODE_BINARY : BS_NODE_UNARY,
                              binary ? bsJSONBinaryMember : bsJSONUnaryMember, &context);
        if (node == 0 || ast->nodes[node].op == 0 || ast->nodes[node].a == 0 || (binary && ast->nodes[node].b == 0)) {
            return 0;
        }
        break;
    }
    default:
        return 0;
    }
    return bsJSONSkipTake(parser, '}') ? node : 0;
}


/* A statement under construction: its node collecting its "a" list, and a function's argument names, its "b" */
typedef struct BSJSONStatement {
    BSJSONNode node;
    BSJSONNode args;
    bool statements; /* a function's statements were given */
} BSJSONStatement;


static BS_NOINLINE bool bsJSONReadArgNameElement(BSJSONParser *parser, int depth, BSJSONNode *context)
{
    uint32_t text;
    if (!bsJSONReadText(parser, context->ast, &text)) {
        return false;
    }
    uint32_t arg = bsAstNode(context->ast, BS_NODE_ARG);
    context->ast->nodes[arg].text = text;
    bsJSONNodeAppend(context, arg);
    return true;
}


static BS_NOINLINE bool bsJSONReadStatementElement(BSJSONParser *parser, int depth, BSJSONNode *context)
{
    uint32_t statement = bsJSONReadStatement(parser, context->ast, depth);
    if (statement == 0) {
        return false;
    }
    bsJSONNodeAppend(context, statement);
    return true;
}


static BS_NOINLINE bool bsJSONIncludeMember(BSJSONParser *parser, BSModelKey key, int depth, void *data)
{
    BSJSONNode *context = data;
    BSAst *ast = context->ast;
    BSValue value;
    switch (key) {
    case BS_MKEY_URL:
        return bsJSONReadText(parser, ast, &ast->nodes[context->node].text);
    case BS_MKEY_SYSTEM:
        if (!bsJSONDecodeValue(parser, depth, &value)) {
            return false;
        }
        ast->nodes[context->node].flag = bsValueBoolean(value);
        bsRelease(value);
        return true;
    default:
        return bsJSONSkipValue(parser, depth);
    }
}


static BS_NOINLINE bool bsJSONReadIncludeElement(BSJSONParser *parser, int depth, BSJSONNode *context)
{
    BSJSONNode itemContext = {.ast = context->ast};
    uint32_t item = bsJSONReadNode(parser, depth, BS_NODE_INCLUDE_ITEM, bsJSONIncludeMember, &itemContext);
    if (item == 0 || context->ast->nodes[item].text == 0) {
        return false;
    }
    bsJSONNodeAppend(context, item);
    return true;
}


static BS_NOINLINE bool bsJSONStatementMember(BSJSONParser *parser, BSModelKey key, int depth, void *data)
{
    BSJSONStatement *context = data;
    BSAst *ast = context->node.ast;
    uint32_t node = context->node.node;
    BSValue value;
    switch (ast->nodes[node].kind) {
    case BS_NODE_EXPR:
        if (key == BS_MKEY_EXPR) {
            return bsJSONReadExprInto(parser, ast, depth, node, false);
        }
        if (key == BS_MKEY_NAME) {
            /* A name that is not a string is no name */
            if (bsJSONPeek(parser, '"')) {
                return bsJSONReadText(parser, ast, &ast->nodes[node].text);
            }
            return bsJSONSkipValue(parser, depth);
        }
        break;
    case BS_NODE_JUMP:
        if (key == BS_MKEY_LABEL) {
            return bsJSONReadText(parser, ast, &ast->nodes[node].text);
        }
        if (key == BS_MKEY_EXPR) {
            return bsJSONReadExprInto(parser, ast, depth, node, false);
        }
        break;
    case BS_NODE_RETURN:
        if (key == BS_MKEY_EXPR) {
            return bsJSONReadExprInto(parser, ast, depth, node, false);
        }
        break;
    case BS_NODE_LABEL:
        if (key == BS_MKEY_NAME) {
            return bsJSONReadText(parser, ast, &ast->nodes[node].text);
        }
        break;
    case BS_NODE_FUNCTION:
        if (key == BS_MKEY_NAME) {
            return bsJSONReadText(parser, ast, &ast->nodes[node].text);
        }
        if (key == BS_MKEY_ARGS) {
            /* Argument names that are not an array are no arguments */
            if (!bsJSONPeek(parser, '[')) {
                return bsJSONSkipValue(parser, depth);
            }
            return bsJSONReadElements(parser, depth, bsJSONReadArgNameElement, &context->args) &&
                !context->args.failed;
        }
        if (key == BS_MKEY_STATEMENTS) {
            if (!bsJSONPeek(parser, '[')) {
                return false;
            }
            context->statements = true;
            return bsJSONReadElements(parser, depth, bsJSONReadStatementElement, &context->node) &&
                !context->node.failed;
        }
        if (key == BS_MKEY_LAST_ARG_ARRAY) {
            if (!bsJSONDecodeValue(parser, depth, &value)) {
                return false;
            }
            ast->nodes[node].flag = bsValueBoolean(value);
            bsRelease(value);
            return true;
        }
        break;
    default:
        /* BS_NODE_INCLUDE */
        if (key == BS_MKEY_INCLUDES) {
            if (!bsJSONPeek(parser, '[')) {
                return false;
            }
            return bsJSONReadElements(parser, depth, bsJSONReadIncludeElement, &context->node) && !context->node.failed;
        }
        break;
    }
    if (key == BS_MKEY_LINE_NUMBER) {
        /* A line number of another shape is none */
        if (!bsJSONDecodeValue(parser, depth, &value)) {
            return false;
        }
        ast->nodes[node].line = value.type == BS_NUMBER ? (int) value.u.number : 0;
        bsRelease(value);
        return true;
    }
    return bsJSONSkipValue(parser, depth);
}


/*
 * Read a statement - an object of one member whose key is its kind and whose value is the
 * statement object - into the arena. Returns the node, or zero as bsJSONReadExpr does. The
 * depth is checked by the array that holds the statement, which is nearer the limit.
 */
static uint32_t bsJSONReadStatement(BSJSONParser *parser, BSAst *ast, int depth)
{
    if (!bsJSONSkipTake(parser, '{')) {
        return 0;
    }
    const char *data;
    size_t size;
    BSValue decoded;
    if (!bsJSONReadKey(parser, &data, &size, &decoded)) {
        return 0;
    }
    BSModelKey key = bsModelKeyOf(data, size);
    bsRelease(decoded);

    /* The statement kinds, by their keys */
    static const uint8_t kinds[] = {
        [BS_MKEY_EXPR] = BS_NODE_EXPR, [BS_MKEY_JUMP] = BS_NODE_JUMP, [BS_MKEY_RETURN] = BS_NODE_RETURN,
        [BS_MKEY_LABEL] = BS_NODE_LABEL, [BS_MKEY_FUNCTION] = BS_NODE_FUNCTION, [BS_MKEY_INCLUDE] = BS_NODE_INCLUDE
    };
    uint8_t kind = key < sizeof(kinds) ? kinds[key] : 0;
    if (kind == 0) {
        return 0;
    }
    uint32_t node = bsAstNode(ast, kind);
    BSJSONStatement context = {.node = {.ast = ast, .node = node}, .args = {.ast = ast, .node = node, .second = true}};
    if (!bsJSONReadMembers(parser, depth + 1, bsJSONStatementMember, &context)) {
        return 0;
    }

    /* The members a statement must have */
    const BSNode *built = &ast->nodes[node];
    bool complete;
    switch (kind) {
    case BS_NODE_EXPR:
        complete = built->a != 0;
        break;
    case BS_NODE_JUMP:
    case BS_NODE_LABEL:
        complete = built->text != 0;
        break;
    case BS_NODE_RETURN:
        complete = true;
        break;
    case BS_NODE_FUNCTION:
        complete = built->text != 0 && context.statements;
        break;
    default:
        complete = built->a != 0;
        break;
    }
    return complete && bsJSONSkipTake(parser, '}') ? node : 0;
}


/* The statements array at the offset - each statement read, emitted, and dropped from the arena */
static bool bsJSONReadStatements(BSJSONParser *parser, BSAst *ast, bool (*emit)(BSAst *, uint32_t, void *),
                                 void *data)
{
    parser->offset++;
    if (bsJSONSkipTake(parser, ']')) {
        return true;
    }
    while (true) {
        bsAstReset(ast);
        bsJSONSkipSpace(parser);
        size_t begin = parser->offset;
        uint32_t statement = bsJSONReadStatement(parser, ast, 2);
        if (statement == 0) {
            if (parser->error != NULL) {
                return false;
            }
            /* Not a statement's shape: as any value, a syntax error inside reports as one */
            parser->offset = begin;
            if (!bsJSONSkipValue(parser, 2)) {
                return false;
            }
            return bsJSONError(parser, "Invalid BareScript model", parser->offset);
        }
        if (!emit(ast, statement, data)) {
            return bsJSONError(parser, "Invalid BareScript model", parser->offset);
        }
        int separator = bsJSONSeparator(parser, ']');
        if (separator < 0) {
            return false;
        }
        if (separator > 0) {
            return true;
        }
    }
}


static bool bsJSONReadScript(BSJSONParser *parser, BSAst *ast, bool (*emit)(BSAst *, uint32_t, void *), void *data,
                             BSValue rest)
{
    if (!bsJSONSkipTake(parser, '{')) {
        return bsJSONError(parser, "Expecting value", parser->offset);
    }
    bool statements = false;
    if (!bsJSONSkipTake(parser, '}')) {
        while (true) {
            const char *keyData;
            size_t keySize;
            BSValue keyDecoded;
            if (!bsJSONReadKey(parser, &keyData, &keySize, &keyDecoded)) {
                return false;
            }
            bsJSONSkipSpace(parser);
            bool isStatements = keySize == 10 && memcmp(keyData, "statements", 10) == 0;
            if (isStatements && parser->offset < parser->size && parser->text[parser->offset] == '[') {
                bsRelease(keyDecoded);
                if (!bsJSONReadStatements(parser, ast, emit, data)) {
                    return false;
                }
                statements = true;
            } else {
                BSValue key = keyDecoded.type == BS_STRING ? keyDecoded : bsStringInternExisting(keyData, keySize);
                BSValue item;
                if (!bsJSONDecodeValue(parser, 1, &item)) {
                    bsRelease(key);
                    return false;
                }
                bsObjectSetString(rest, key, item);
                bsRelease(key);
            }
            int separator = bsJSONSeparator(parser, '}');
            if (separator < 0) {
                return false;
            }
            if (separator > 0) {
                break;
            }
        }
    }
    if (!statements) {
        return bsJSONError(parser, "Invalid BareScript model", parser->offset);
    }
    bsJSONSkipSpace(parser);
    if (parser->offset != parser->size) {
        return bsJSONError(parser, "Extra data", parser->offset);
    }
    return true;
}


bool bsJSONDecodeScript(const char *text, size_t size, BSAst *ast,
                        bool (*emit)(BSAst *ast, uint32_t statement, void *data), void *data,
                        BSValue *rest, const char **error)
{
    bsModelKeysInit();
    BSJSONParser parser = {text, size, 0, NULL, 0};
    *rest = bsObjectNew();
    bool decoded = bsJSONReadScript(&parser, ast, emit, data, *rest);
    if (!decoded) {
        bsRelease(*rest);
        *rest = bsNull();
    }
    if (error != NULL) {
        *error = decoded ? NULL : parser.error;
    }
    return decoded;
}
