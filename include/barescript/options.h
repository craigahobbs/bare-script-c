/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * BareScript runtime option function implementations
 */

#ifndef BARESCRIPT_OPTIONS_H
#define BARESCRIPT_OPTIONS_H

#include "runtime.h"
#include "export.h"

#ifdef __cplusplus
extern "C" {
#endif

BS_VISIBILITY_BEGIN


/* A fetch function that reads and writes the local file system, and fetches HTTP(S) URLs */
void bsFetchReadWrite(const BSFetchRequest *requests, BSValue *responses, size_t count, void *data);

/* A fetch function that reads the local file system, and fetches HTTP(S) URLs */
void bsFetchReadOnly(const BSFetchRequest *requests, BSValue *responses, size_t count, void *data);

/*
 * A fetch function that fetches http and https URLs with libcurl - the schemes a browser's fetch
 * accepts. A batch's requests are fetched concurrently over the calling thread's connection pool,
 * multiplexed where an HTTPS server speaks HTTP/2. A request of any other scheme fails, as does a
 * redirect to one, and every request fails unless the library was built with libcurl support
 * (BARESCRIPT_CURL) and libcurl loads at runtime.
 */
void bsFetchHTTP(const BSFetchRequest *requests, BSValue *responses, size_t count, void *data);

/* True if the library was built with libcurl HTTP support, and libcurl loads at runtime */
bool bsFetchHTTPAvailable(void);

/* A log function that writes to stdout */
void bsLogStdout(const char *text, void *data);

/*
 * A URL function that resolves a URL or POSIX path relative to a file path. "data" is the
 * NUL-terminated file path. Returns a malloc-allocated string.
 */
char *bsUrlFileRelative(const char *url, void *data);


BS_VISIBILITY_END

#ifdef __cplusplus
}
#endif

#endif
