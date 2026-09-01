/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * BareScript runtime option function implementations
 */

#ifndef BARESCRIPT_OPTIONS_H
#define BARESCRIPT_OPTIONS_H

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif


/* A fetch function that reads and writes the local file system, and fetches HTTP(S) URLs */
char *bsFetchReadWrite(const BSFetchRequest *request, size_t *responseSize, void *data);

/* A fetch function that reads the local file system, and fetches HTTP(S) URLs */
char *bsFetchReadOnly(const BSFetchRequest *request, size_t *responseSize, void *data);

/*
 * A fetch function that fetches HTTP(S) URLs with libcurl. Returns NULL for non-URL requests, and
 * always returns NULL unless the library was built with libcurl support (BARESCRIPT_CURL).
 */
char *bsFetchHTTP(const BSFetchRequest *request, size_t *responseSize, void *data);

/* True if the library was built with libcurl HTTP support */
bool bsFetchHTTPAvailable(void);

/* A log function that writes to stdout */
void bsLogStdout(const char *text, void *data);

/*
 * A URL function that resolves a URL or POSIX path relative to a file path. "data" is the
 * NUL-terminated file path. Returns a malloc-allocated string.
 */
char *bsUrlFileRelative(const char *url, void *data);

/* True if the text begins with a URL scheme ("http:", "file:", ...) */
bool bsUrlIsURL(const char *url);


#ifdef __cplusplus
}
#endif

#endif
