/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The bundled BareScript include library
 *
 * Each include library script is embedded in the library as its parser-compiled JSON script model,
 * dictionary compressed with a phrase table (see bin/includeSource.bare). A model is decoded on
 * first use and cached, so a program that includes two of the thirty bundled scripts pays for two.
 */

#include <string.h>

#include "barescript/includeSource.h"
#include "barescript/runtime.h"

#include "includeSourceDecode.h"
#include "internal.h"


const char *bsIncludeSourceDecode(BSIncludeSource *source)
{
    if (source->decoded != NULL) {
        return source->decoded;
    }

    /*
     * Decode the compressed model
     *
     * The compressed text is a run of literal characters, then for each "~" an index character
     * naming a phrase, then more literal characters. The index one past the last phrase is the
     * escape for a literal "~".
     */
    /* Size the result, so the output buffer is allocated once */
    size_t decodedSize = 0;
    for (const char *const *chunk = source->compressed; *chunk != NULL; chunk++) {
        const char *text = *chunk;
        for (size_t ix = 0; text[ix] != '\0'; ix++) {
            if (text[ix] != '~') {
                decodedSize++;
                continue;
            }
            decodedSize += strlen(bsIncludeSourcePhrases[bsIncludeSourcePhraseIndex(text[ix + 1])]);
            ix++;
        }
    }

    char *decoded = bsAlloc(decodedSize + 1);
    size_t offset = 0;
    for (const char *const *chunk = source->compressed; *chunk != NULL; chunk++) {
        const char *text = *chunk;
        for (size_t ix = 0; text[ix] != '\0'; ix++) {
            if (text[ix] != '~') {
                decoded[offset++] = text[ix];
                continue;
            }
            const char *phrase = bsIncludeSourcePhrases[bsIncludeSourcePhraseIndex(text[ix + 1])];
            size_t phraseSize = strlen(phrase);
            memcpy(decoded + offset, phrase, phraseSize);
            offset += phraseSize;
            ix++;
        }
    }
    decoded[decodedSize] = '\0';

    source->decoded = decoded;
    return decoded;
}


size_t bsIncludeSourcePhraseIndex(char index)
{
    /* The index alphabet is "a-zA-Z0-9" - the phrase table's order */
    if (index >= 'a' && index <= 'z') {
        return (size_t) (index - 'a');
    }
    if (index >= 'A' && index <= 'Z') {
        return (size_t) (index - 'A') + 26;
    }
    return (size_t) (index - '0') + 52;
}


size_t bsIncludeCount(void)
{
    return BS_INCLUDE_COUNT;
}


const char *bsIncludeName(size_t index)
{
    return index < BS_INCLUDE_COUNT ? bsIncludeSources[index].name : NULL;
}


const char *bsIncludeSource(const char *name)
{
    for (size_t ix = 0; ix < BS_INCLUDE_COUNT; ix++) {
        if (strcmp(bsIncludeSources[ix].name, name) == 0) {
            return bsIncludeSourceDecode(&bsIncludeSources[ix]);
        }
    }
    return NULL;
}


void bsIncludeCleanup(void)
{
    for (size_t ix = 0; ix < BS_INCLUDE_COUNT; ix++) {
        free(bsIncludeSources[ix].decoded);
        bsIncludeSources[ix].decoded = NULL;
    }
}
