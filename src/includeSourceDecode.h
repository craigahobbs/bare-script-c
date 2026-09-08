/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The bundled include library decoder - gzip, internal to the include source implementation
 */

#ifndef BARESCRIPT_INCLUDE_SOURCE_DECODE_H
#define BARESCRIPT_INCLUDE_SOURCE_DECODE_H

#include "barescript/includeSource.h"


/* Inflate a bundled include's compressed binary model by registry index, caching the bytes for the thread */
const unsigned char *bsIncludeSourceDecode(size_t index, size_t *size);


#endif
