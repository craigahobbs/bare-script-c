/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * A targeted JSON encoder/decoder for BareScript
 *
 * The encoder writes BareScript values as JSON with object keys in sorted order - which the value
 * system provides for free, since objects are key-ordered binary search trees. Datetime and
 * function values encode as their bsValueString representation; regex values encode as null.
 */

#ifndef BARESCRIPT_JSON_H
#define BARESCRIPT_JSON_H

#include "value.h"

#ifdef __cplusplus
extern "C" {
#endif


/*
 * Encode a value as JSON. If "indent" is greater than zero, the JSON is pretty-printed with that
 * many spaces of indentation per level. Returns an owned string value.
 */
BSValue bsJSONEncode(BSValue value, int indent);

/* Encode a value as JSON, appending to a string builder */
void bsJSONEncodeSB(BSStringBuilder *sb, BSValue value, int indent);

/*
 * Decode a JSON string. Returns an owned value, or a null value on error. If "error" is non-NULL,
 * it is set to a static description of the decoding error (NULL on success).
 */
BSValue bsJSONDecode(const char *text, size_t size, const char **error);

/* Decode a JSON string, additionally reporting the byte offset the error was detected at */
BSValue bsJSONDecodeEx(const char *text, size_t size, const char **error, size_t *errorOffset);


#ifdef __cplusplus
}
#endif

#endif
