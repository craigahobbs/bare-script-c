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


typedef struct BSJSONParser {
    const char *text;
    size_t size;
    size_t offset;
    const char *error;
} BSJSONParser;


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


static bool bsJSONDecodeString(BSJSONParser *parser, BSValue *result)
{
    if (parser->offset >= parser->size || parser->text[parser->offset] != '"') {
        parser->error = "Expected a string";
        return false;
    }
    parser->offset++;

    BSStringBuilder sb;
    bsSBInit(&sb);
    char utf8[4];
    while (true) {
        if (parser->offset >= parser->size) {
            parser->error = "Unterminated string";
            bsSBFree(&sb);
            return false;
        }
        char ch = parser->text[parser->offset];
        if (ch == '"') {
            parser->offset++;
            break;
        }
        if (ch != '\\') {
            if ((unsigned char) ch < 0x20) {
                parser->error = "Invalid string control character";
                bsSBFree(&sb);
                return false;
            }
            bsSBAppendChar(&sb, ch);
            parser->offset++;
            continue;
        }

        parser->offset++;
        if (parser->offset >= parser->size) {
            parser->error = "Unterminated string escape";
            bsSBFree(&sb);
            return false;
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
                parser->error = "Invalid unicode escape";
                bsSBFree(&sb);
                return false;
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
            parser->error = "Invalid string escape";
            bsSBFree(&sb);
            return false;
        }
    }

    *result = bsSBToValue(&sb);
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
    while (true) {
        BSValue item;
        if (!bsJSONDecodeValue(parser, depth + 1, &item)) {
            bsRelease(array);
            return false;
        }
        bsArrayPush(array, item);
        bsJSONSkipSpace(parser);
        if (parser->offset >= parser->size) {
            parser->error = "Unterminated array";
            bsRelease(array);
            return false;
        }
        char ch = parser->text[parser->offset++];
        if (ch == ']') {
            break;
        }
        if (ch != ',') {
            parser->error = "Expected ',' or ']'";
            bsRelease(array);
            return false;
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
        if (!bsJSONDecodeString(parser, &key)) {
            bsRelease(object);
            return false;
        }
        bsJSONSkipSpace(parser);
        if (parser->offset >= parser->size || parser->text[parser->offset] != ':') {
            parser->error = "Expected ':'";
            bsRelease(key);
            bsRelease(object);
            return false;
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
        if (parser->offset >= parser->size) {
            parser->error = "Unterminated object";
            bsRelease(object);
            return false;
        }
        char ch = parser->text[parser->offset++];
        if (ch == '}') {
            break;
        }
        if (ch != ',') {
            parser->error = "Expected ',' or '}'";
            bsRelease(object);
            return false;
        }
    }
    *result = object;
    return true;
}


static bool bsJSONDecodeNumber(BSJSONParser *parser, BSValue *result)
{
    size_t begin = parser->offset;
    if (parser->offset < parser->size && parser->text[parser->offset] == '-') {
        parser->offset++;
    }
    size_t digits = 0;
    while (parser->offset < parser->size && isdigit((unsigned char) parser->text[parser->offset])) {
        parser->offset++;
        digits++;
    }
    if (digits == 0) {
        parser->error = "Invalid number";
        return false;
    }
    if (parser->offset < parser->size && parser->text[parser->offset] == '.') {
        parser->offset++;
        size_t fractionDigits = 0;
        while (parser->offset < parser->size && isdigit((unsigned char) parser->text[parser->offset])) {
            parser->offset++;
            fractionDigits++;
        }
        if (fractionDigits == 0) {
            parser->error = "Invalid number";
            return false;
        }
    }
    if (parser->offset < parser->size &&
        (parser->text[parser->offset] == 'e' || parser->text[parser->offset] == 'E')) {
        parser->offset++;
        if (parser->offset < parser->size &&
            (parser->text[parser->offset] == '-' || parser->text[parser->offset] == '+')) {
            parser->offset++;
        }
        size_t exponentDigits = 0;
        while (parser->offset < parser->size && isdigit((unsigned char) parser->text[parser->offset])) {
            parser->offset++;
            exponentDigits++;
        }
        if (exponentDigits == 0) {
            parser->error = "Invalid number";
            return false;
        }
    }

    char buffer[64];
    size_t size = parser->offset - begin;
    if (size >= sizeof(buffer)) {
        parser->error = "Invalid number";
        return false;
    }
    memcpy(buffer, parser->text + begin, size);
    buffer[size] = '\0';
    *result = bsNumber(strtod(buffer, NULL));
    return true;
}


static bool bsJSONDecodeValue(BSJSONParser *parser, int depth, BSValue *result)
{
    if (depth >= BS_JSON_DEPTH_MAX) {
        parser->error = "Maximum nesting depth exceeded";
        return false;
    }
    bsJSONSkipSpace(parser);
    if (parser->offset >= parser->size) {
        parser->error = "Unexpected end of input";
        return false;
    }
    char ch = parser->text[parser->offset];
    if (ch == '{') {
        return bsJSONDecodeObject(parser, depth, result);
    }
    if (ch == '[') {
        return bsJSONDecodeArray(parser, depth, result);
    }
    if (ch == '"') {
        return bsJSONDecodeString(parser, result);
    }
    if (ch == 't') {
        if (!bsJSONLiteral(parser, "true")) {
            parser->error = "Invalid value";
            return false;
        }
        *result = bsBoolean(true);
        return true;
    }
    if (ch == 'f') {
        if (!bsJSONLiteral(parser, "false")) {
            parser->error = "Invalid value";
            return false;
        }
        *result = bsBoolean(false);
        return true;
    }
    if (ch == 'n') {
        if (!bsJSONLiteral(parser, "null")) {
            parser->error = "Invalid value";
            return false;
        }
        *result = bsNull();
        return true;
    }
    if (ch == '-' || isdigit((unsigned char) ch)) {
        return bsJSONDecodeNumber(parser, result);
    }
    parser->error = "Invalid value";
    return false;
}


BSValue bsJSONDecode(const char *text, size_t size, const char **error)
{
    BSJSONParser parser = {text, size, 0, NULL};
    BSValue result;
    if (!bsJSONDecodeValue(&parser, 0, &result)) {
        if (error != NULL) {
            *error = parser.error;
        }
        return bsNull();
    }
    bsJSONSkipSpace(&parser);
    if (parser.offset != parser.size) {
        bsRelease(result);
        if (error != NULL) {
            *error = "Unexpected trailing text";
        }
        return bsNull();
    }
    if (error != NULL) {
        *error = NULL;
    }
    return result;
}
