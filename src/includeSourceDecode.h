/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The bundled include library decoder - gzip, internal to the include source implementation
 */

#ifndef BARESCRIPT_INCLUDE_SOURCE_DECODE_H
#define BARESCRIPT_INCLUDE_SOURCE_DECODE_H

#include <stdlib.h>

#include "barescript/includeSource.h"


/* Decode a bundled include's compressed model, caching the result on the registry entry */
const char *bsIncludeSourceDecode(BSIncludeSource *source);


#endif
