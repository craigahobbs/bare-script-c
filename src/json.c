/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * A targeted JSON encoder/decoder for BareScript
 */

#include <ctype.h>
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


/* Decode the string at the offset, which holds its opening quote */
static bool bsJSONDecodeString(BSJSONParser *parser, BSValue *result, bool asKey)
{
    size_t begin = parser->offset++;

    /* Unescaped strings copy once from the input; escapes fall through to the builder */
    size_t ix = parser->offset;
    while (ix < parser->size) {
        unsigned char ch = (unsigned char) parser->text[ix];
        if (ch == '"') {
            const char *data = parser->text + parser->offset;
            size_t size = ix - parser->offset;
            *result = asKey ? bsStringInternExisting(data, size) : bsStringNewSize(data, size);
            parser->offset = ix + 1;
            return true;
        }
        if (ch == '\\' || ch < 0x20) {
            break;
        }
        ix++;
    }

    BSStringBuilder sb;
    bsSBInit(&sb);
    char utf8[4];
    while (true) {
        if (parser->offset >= parser->size) {
            bsSBFree(&sb);
            return bsJSONError(parser, "Unterminated string starting at", begin);
        }
        char ch = parser->text[parser->offset];
        if (ch == '"') {
            parser->offset++;
            break;
        }
        if (ch != '\\') {
            if ((unsigned char) ch < 0x20) {
                bsSBFree(&sb);
                return bsJSONError(parser, "Invalid control character at", parser->offset);
            }
            bsSBAppendChar(&sb, ch);
            parser->offset++;
            continue;
        }

        size_t escapeOffset = parser->offset;
        parser->offset++;
        if (parser->offset >= parser->size) {
            bsSBFree(&sb);
            return bsJSONError(parser, "Unterminated string starting at", begin);
        }
        char escape = parser->text[parser->offset++];
        if (bsJSONUnescape[(unsigned char) escape] != '\0') {
            bsSBAppendChar(&sb, bsJSONUnescape[(unsigned char) escape]);
        } else if (escape == 'u') {
            uint32_t codePoint;
            if (!bsJSONHex4(parser, &codePoint)) {
                bsSBFree(&sb);
                return bsJSONError(parser, "Invalid \\uXXXX escape", escapeOffset + 1);
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
            bsSBFree(&sb);
            return bsJSONError(parser, "Invalid \\escape", escapeOffset);
        }
    }

    *result = asKey ? bsStringInternExisting(sb.data, sb.size) : bsStringNewSize(sb.data, sb.size);
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
    bsJSONSkipSpace(parser);
    if (parser->offset < parser->size && parser->text[parser->offset] == close) {
        parser->offset++;
        return 1;
    }
    if (parser->offset >= parser->size || parser->text[parser->offset] != ',') {
        bsJSONError(parser, "Expecting ',' delimiter", parser->offset);
        return -1;
    }
    size_t commaOffset = parser->offset;
    parser->offset++;
    bsJSONSkipSpace(parser);
    if (parser->offset < parser->size && parser->text[parser->offset] == close) {
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
    bsJSONSkipSpace(parser);
    if (parser->offset < parser->size && parser->text[parser->offset] == ']') {
        parser->offset++;
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
    if (!bsJSONDecodeString(parser, key, true)) {
        return false;
    }
    bsJSONSkipSpace(parser);
    if (parser->offset >= parser->size || parser->text[parser->offset] != ':') {
        bsRelease(*key);
        return bsJSONError(parser, "Expecting ':' delimiter", parser->offset);
    }
    parser->offset++;
    return true;
}


static bool bsJSONDecodeObject(BSJSONParser *parser, int depth, BSValue *result)
{
    parser->offset++;
    BSValue object = bsObjectNew();
    bsJSONSkipSpace(parser);
    if (parser->offset < parser->size && parser->text[parser->offset] == '}') {
        parser->offset++;
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
        while (ix < size && isdigit((unsigned char) text[ix])) {
            ix++;
        }
    } else {
        return bsJSONError(parser, "Expecting value", begin);
    }
    if (ix + 1 < size && text[ix] == '.' && isdigit((unsigned char) text[ix + 1])) {
        ix += 2;
        while (ix < size && isdigit((unsigned char) text[ix])) {
            ix++;
        }
    }
    if (ix < size && (text[ix] == 'e' || text[ix] == 'E')) {
        size_t save = ix;
        ix++;
        if (ix < size && (text[ix] == '-' || text[ix] == '+')) {
            ix++;
        }
        if (ix < size && isdigit((unsigned char) text[ix])) {
            while (ix < size && isdigit((unsigned char) text[ix])) {
                ix++;
            }
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
        return bsJSONDecodeString(parser, result, false);
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
    if (ch == '-' || isdigit((unsigned char) ch)) {
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


static bool bsJSONDecodeStatementArray(BSJSONParser *parser, bool (*emit)(BSValue, void *), void *data)
{
    parser->offset++;
    bsJSONSkipSpace(parser);
    if (parser->offset < parser->size && parser->text[parser->offset] == ']') {
        parser->offset++;
        return true;
    }
    while (true) {
        BSValue statement;
        if (!bsJSONDecodeValue(parser, 2, &statement)) {
            return false;
        }
        bool emitted = emit(statement, data);
        bsRelease(statement);
        if (!emitted) {
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


static bool bsJSONDecodeModel(BSJSONParser *parser, bool (*emit)(BSValue, void *), void *data, BSValue rest)
{
    bsJSONSkipSpace(parser);
    if (parser->offset >= parser->size || parser->text[parser->offset] != '{') {
        return bsJSONError(parser, "Expecting value", parser->offset);
    }
    parser->offset++;
    bsJSONSkipSpace(parser);
    bool statements = false;
    if (parser->offset < parser->size && parser->text[parser->offset] == '}') {
        parser->offset++;
    } else {
        while (true) {
            BSValue key;
            if (!bsJSONDecodeKey(parser, &key)) {
                return false;
            }
            bsJSONSkipSpace(parser);
            bool isStatements = bsStringSize(key) == 10 && memcmp(bsStringData(key), "statements", 10) == 0;
            if (isStatements && parser->offset < parser->size && parser->text[parser->offset] == '[') {
                bsRelease(key);
                if (!bsJSONDecodeStatementArray(parser, emit, data)) {
                    return false;
                }
                statements = true;
            } else {
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


bool bsJSONDecodeStatements(const char *text, size_t size, bool (*emit)(BSValue statement, void *data),
                            void *data, BSValue *rest, const char **error)
{
    bsModelKeysInit();
    BSJSONParser parser = {text, size, 0, NULL, 0};
    *rest = bsObjectNew();
    bool decoded = bsJSONDecodeModel(&parser, emit, data, *rest);
    if (!decoded) {
        bsRelease(*rest);
        *rest = bsNull();
    }
    if (error != NULL) {
        *error = decoded ? NULL : parser.error;
    }
    return decoded;
}
