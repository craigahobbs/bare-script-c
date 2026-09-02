/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * A targeted JSON encoder/decoder for BareScript
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "barescript/json.h"

#include "internal.h"


/* The maximum JSON nesting depth */
#define BS_JSON_DEPTH_MAX 1000


/*
 * Encode
 */


static void bsJSONEncodeString(BSStringBuilder *sb, BSValue value)
{
    const char *data = bsStringData(value);
    size_t size = bsStringSize(value);
    bsSBAppendChar(sb, '"');
    size_t begin = 0;
    for (size_t ix = 0; ix < size; ix++) {
        unsigned char ch = (unsigned char) data[ix];
        const char *escape = NULL;
        char buffer[8];
        switch (ch) {
        case '"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        case '\b':
            escape = "\\b";
            break;
        case '\f':
            escape = "\\f";
            break;
        default:
            if (ch < 0x20) {
                snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                escape = buffer;
            }
            break;
        }
        if (escape != NULL) {
            bsSBAppend(sb, data + begin, ix - begin);
            bsSBAppendString(sb, escape);
            begin = ix + 1;
        }
    }
    bsSBAppend(sb, data + begin, size - begin);
    bsSBAppendChar(sb, '"');
}


static void bsJSONIndent(BSStringBuilder *sb, int indent, int depth)
{
    if (indent > 0) {
        bsSBAppendChar(sb, '\n');
        for (int ix = 0; ix < indent * depth; ix++) {
            bsSBAppendChar(sb, ' ');
        }
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
    char buffer[64];

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
            bsNumberFormat(value.u.number, buffer, sizeof(buffer));
            bsSBAppendString(sb, buffer);
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


void bsJSONEncodeSB(BSStringBuilder *sb, BSValue value, int indent)
{
    bsJSONEncodeValue(sb, value, indent, 0);
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
 */


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
        char ch = parser->text[parser->offset + ix];
        uint32_t digit;
        if (ch >= '0' && ch <= '9') {
            digit = (uint32_t) (ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = (uint32_t) (ch - 'a' + 10);
        } else if (ch >= 'A' && ch <= 'F') {
            digit = (uint32_t) (ch - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | digit;
    }
    parser->offset += 4;
    *result = value;
    return true;
}


static bool bsJSONDecodeString(BSJSONParser *parser, BSValue *result, int asKey)
{
    size_t begin = parser->offset;
    if (begin >= parser->size || parser->text[begin] != '"') {
        return bsJSONError(parser, "Expecting property name enclosed in double quotes", begin);
    }
    parser->offset++;

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
        switch (escape) {
        case '"':
            bsSBAppendChar(&sb, '"');
            break;
        case '\\':
            bsSBAppendChar(&sb, '\\');
            break;
        case '/':
            bsSBAppendChar(&sb, '/');
            break;
        case 'b':
            bsSBAppendChar(&sb, '\b');
            break;
        case 'f':
            bsSBAppendChar(&sb, '\f');
            break;
        case 'n':
            bsSBAppendChar(&sb, '\n');
            break;
        case 'r':
            bsSBAppendChar(&sb, '\r');
            break;
        case 't':
            bsSBAppendChar(&sb, '\t');
            break;
        case 'u': {
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
            break;
        }
        default:
            bsSBFree(&sb);
            return bsJSONError(parser, "Invalid \\escape", escapeOffset);
        }
    }

    if (asKey) {
        *result = bsStringInternExisting(sb.data != NULL ? sb.data : "", sb.size);
        bsSBFree(&sb);
    } else {
        *result = bsSBToValue(&sb);
    }
    return true;
}


static bool bsJSONDecodeValue(BSJSONParser *parser, int depth, BSValue *result);


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
    bsArrayReserve(array, 8);
    while (true) {
        BSValue item;
        if (!bsJSONDecodeValue(parser, depth + 1, &item)) {
            bsRelease(array);
            return false;
        }
        bsArrayPush(array, item);
        bsJSONSkipSpace(parser);
        if (parser->offset < parser->size && parser->text[parser->offset] == ']') {
            parser->offset++;
            break;
        }
        if (parser->offset >= parser->size || parser->text[parser->offset] != ',') {
            bsRelease(array);
            return bsJSONError(parser, "Expecting ',' delimiter", parser->offset);
        }
        size_t commaOffset = parser->offset;
        parser->offset++;
        bsJSONSkipSpace(parser);
        if (parser->offset < parser->size && parser->text[parser->offset] == ']') {
            bsRelease(array);
            return bsJSONError(parser, "Illegal trailing comma before end of array", commaOffset);
        }
    }
    *result = array;
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
        bsJSONSkipSpace(parser);
        BSValue key;
        if (!bsJSONDecodeString(parser, &key, 1)) {
            bsRelease(object);
            return false;
        }
        bsJSONSkipSpace(parser);
        if (parser->offset >= parser->size || parser->text[parser->offset] != ':') {
            bsRelease(key);
            bsRelease(object);
            return bsJSONError(parser, "Expecting ':' delimiter", parser->offset);
        }
        parser->offset++;
        BSValue item;
        if (!bsJSONDecodeValue(parser, depth + 1, &item)) {
            bsRelease(key);
            bsRelease(object);
            return false;
        }
        bsObjectSetString(object, key, item);
        bsRelease(key);
        bsJSONSkipSpace(parser);
        if (parser->offset < parser->size && parser->text[parser->offset] == '}') {
            parser->offset++;
            break;
        }
        if (parser->offset >= parser->size || parser->text[parser->offset] != ',') {
            bsRelease(object);
            return bsJSONError(parser, "Expecting ',' delimiter", parser->offset);
        }
        size_t commaOffset = parser->offset;
        parser->offset++;
        bsJSONSkipSpace(parser);
        if (parser->offset < parser->size && parser->text[parser->offset] == '}') {
            bsRelease(object);
            return bsJSONError(parser, "Illegal trailing comma before end of object", commaOffset);
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

    /*
     * strtod needs a terminated string and the decoder's text is not terminated, so copy the
     * number out. Almost every number fits the stack buffer; a longer one - a very long run of
     * digits - takes a heap copy.
     */
    char buffer[64];
    size_t numberSize = ix - begin;
    char *number = buffer;
    if (numberSize >= sizeof(buffer)) {
        number = bsAlloc(numberSize + 1);
    }
    memcpy(number, text + begin, numberSize);
    number[numberSize] = '\0';
    *result = bsNumber(strtod(number, NULL));
    if (number != buffer) {
        free(number);
    }
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
        return bsJSONDecodeString(parser, result, 0);
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
 * Intern the closed set of script-model object keys before decoding JSON. JSON insert reuses
 * interned names so later interned lookup stays pointer-only. Unique payload keys stay ordinary
 * strings and do not grow the intern table.
 */
static void bsJSONInternModelKeys(void)
{
    static int done;
    if (done) {
        return;
    }
    done = 1;
    static const char *const keys[] = {
        "args", "async", "binary", "expr", "function", "group", "include", "includes",
        "jump", "label", "lastArgArray", "left", "lineCount", "lineNumber", "name",
        "number", "op", "return", "right", "scriptLines", "scriptName", "statements",
        "string", "system", "unary", "url", "variable", NULL
    };
    for (const char *const *key = keys; *key != NULL; key++) {
        bsRelease(bsStringIntern(*key, strlen(*key)));
    }
}


BSValue bsJSONDecodeEx(const char *text, size_t size, const char **error, size_t *errorOffset)
{
    bsJSONInternModelKeys();
    BSJSONParser parser = {text, size, 0, NULL, 0};
    BSValue result;
    if (!bsJSONDecodeValue(&parser, 0, &result)) {
        if (error != NULL) {
            *error = parser.error;
        }
        if (errorOffset != NULL) {
            *errorOffset = parser.errorOffset;
        }
        return bsNull();
    }
    bsJSONSkipSpace(&parser);
    if (parser.offset != parser.size) {
        bsRelease(result);
        if (error != NULL) {
            *error = "Extra data";
        }
        if (errorOffset != NULL) {
            *errorOffset = parser.offset;
        }
        return bsNull();
    }
    if (error != NULL) {
        *error = NULL;
    }
    return result;
}


BSValue bsJSONDecode(const char *text, size_t size, const char **error)
{
    return bsJSONDecodeEx(text, size, error, NULL);
}
