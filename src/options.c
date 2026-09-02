/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * BareScript runtime option function implementations
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "barescript/barescript.h"
#include "barescript/options.h"

#include "internal.h"

#ifdef BARESCRIPT_CURL
#include <curl/curl.h>
#endif


const char *bsVersion(void)
{
    return BARESCRIPT_VERSION;
}


bool bsUrlIsURL(const char *url)
{
    /* "^[a-z]+:" - a lower-case scheme followed by a colon */
    size_t ix = 0;
    while (url[ix] >= 'a' && url[ix] <= 'z') {
        ix++;
    }
    return ix != 0 && url[ix] == ':';
}


/* Normalize a file system path - collapse ".", "..", and repeated separators */
static char *bsPathNormalize(const char *path)
{
    size_t size = strlen(path);
    char *result = bsAlloc(size + 2);
    size_t resultSize = 0;
    bool absolute = (size != 0 && path[0] == '/');
    if (absolute) {
        result[resultSize++] = '/';
    }

    /* The segment stack holds each kept segment's start offset in the result */
    size_t *segments = bsAlloc((size + 1) * sizeof(size_t));
    size_t segmentCount = 0;

    size_t ix = 0;
    while (ix < size) {
        while (ix < size && path[ix] == '/') {
            ix++;
        }
        size_t begin = ix;
        while (ix < size && path[ix] != '/') {
            ix++;
        }
        size_t segmentSize = ix - begin;
        if (segmentSize == 0 || (segmentSize == 1 && path[begin] == '.')) {
            continue;
        }
        if (segmentSize == 2 && path[begin] == '.' && path[begin + 1] == '.') {
            /* Pop the previous segment, unless it is itself ".." or the path escapes its root */
            if (segmentCount != 0) {
                size_t previous = segments[segmentCount - 1];
                bool isParent = (resultSize - previous == 3 && memcmp(result + previous, "../", 3) == 0) ||
                    (resultSize - previous == 2 && memcmp(result + previous, "..", 2) == 0);
                if (!isParent) {
                    segmentCount--;
                    resultSize = previous;
                    continue;
                }
            } else if (absolute) {
                continue;
            }
        }
        if (resultSize != 0 && result[resultSize - 1] != '/') {
            result[resultSize++] = '/';
        }
        segments[segmentCount++] = resultSize;
        memcpy(result + resultSize, path + begin, segmentSize);
        resultSize += segmentSize;
    }
    free(segments);

    if (resultSize == 0) {
        result[resultSize++] = '.';
    }
    result[resultSize] = '\0';
    return result;
}


char *bsUrlFileRelative(const char *url, void *data)
{
    const char *file = data;

    /* An absolute URL or POSIX path resolves to itself */
    if (bsUrlIsURL(url) || url[0] == '/') {
        return bsStrdup(url);
    }

    /* The file is a URL - replace its last path segment */
    if (file != NULL && bsUrlIsURL(file)) {
        const char *lastSlash = strrchr(file, '/');
        size_t prefixSize = lastSlash != NULL ? (size_t) (lastSlash - file) + 1 : strlen(file);
        size_t urlSize = strlen(url);
        char *result = bsAlloc(prefixSize + urlSize + 1);
        memcpy(result, file, prefixSize);
        memcpy(result + prefixSize, url, urlSize + 1);
        return result;
    }

    /* The file is a file system path - resolve relative to its directory */
    const char *lastSlash = file != NULL ? strrchr(file, '/') : NULL;
    size_t dirSize = lastSlash != NULL ? (size_t) (lastSlash - file) + 1 : 0;
    size_t urlSize = strlen(url);
    char *joined = bsAlloc(dirSize + urlSize + 1);
    if (dirSize != 0) {
        memcpy(joined, file, dirSize);
    }
    memcpy(joined + dirSize, url, urlSize + 1);
    char *result = bsPathNormalize(joined);
    free(joined);
    return result;
}


void bsLogStdout(const char *text, void *data)
{
    printf("%s\n", text);
}


/*
 * HTTP fetch
 */


#ifdef BARESCRIPT_CURL

typedef struct BSCurlBuffer {
    char *data;
    size_t size;
    size_t capacity;
} BSCurlBuffer;


static size_t bsCurlWrite(char *data, size_t size, size_t count, void *userData)
{
    BSCurlBuffer *buffer = userData;
    size_t total = size * count;
    if (buffer->size + total + 1 > buffer->capacity) {
        size_t capacity = buffer->capacity != 0 ? buffer->capacity : 4096;
        while (capacity < buffer->size + total + 1) {
            capacity *= 2;
        }
        buffer->data = bsRealloc(buffer->data, capacity);
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->size, data, total);
    buffer->size += total;
    buffer->data[buffer->size] = '\0';
    return total;
}


static bool bsCurlHeaderIter(BSValue key, BSValue item, void *data)
{
    struct curl_slist **headers = data;
    BSValue header = bsStringNewFormat("%s: %s", bsStringData(key), bsStringData(item));
    *headers = curl_slist_append(*headers, bsStringData(header));
    bsRelease(header);
    return true;
}


char *bsFetchHTTP(const BSFetchRequest *request, size_t *responseSize, void *data)
{
    if (!bsUrlIsURL(request->url)) {
        return NULL;
    }

    CURL *curl = curl_easy_init();
    /* GCOV_EXCL_START */
    if (curl == NULL) {
        return NULL;
    }
    /* GCOV_EXCL_STOP */

    BSCurlBuffer buffer = {NULL, 0, 0};
    struct curl_slist *headers = NULL;
    bsObjectIter(request->headers, bsCurlHeaderIter, &headers);

    curl_easy_setopt(curl, CURLOPT_URL, request->url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, bsCurlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "bare-script-c");
    if (request->body != NULL) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long) request->bodySize);
    }
    if (headers != NULL) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    CURLcode status = curl_easy_perform(curl);
    long responseCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (status != CURLE_OK || (responseCode != 0 && responseCode != 200)) {
        free(buffer.data);
        return NULL;
    }
    if (buffer.data == NULL) {
        buffer.data = bsAlloc(1);
        buffer.data[0] = '\0';
    }
    if (responseSize != NULL) {
        *responseSize = buffer.size;
    }
    return buffer.data;
}


bool bsFetchHTTPAvailable(void)
{
    return true;
}

#else

char *bsFetchHTTP(const BSFetchRequest *request, size_t *responseSize, void *data)
{
    return NULL;
}


bool bsFetchHTTPAvailable(void)
{
    return false;
}

#endif


/*
 * File system fetch
 */


static char *bsFileRead(const char *path, size_t *responseSize)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    size_t capacity = 4096;
    size_t size = 0;
    char *buffer = bsAlloc(capacity);
    while (true) {
        if (size + 4096 + 1 > capacity) {
            capacity *= 2;
            buffer = bsRealloc(buffer, capacity);
        }
        size_t read = fread(buffer + size, 1, 4096, file);
        size += read;
        if (read != 4096) {
            break;
        }
    }
    bool failed = (ferror(file) != 0);
    fclose(file);
    if (failed) {
        free(buffer); /* GCOV_EXCL_LINE */
        return NULL;  /* GCOV_EXCL_LINE */
    }
    buffer[size] = '\0';
    if (responseSize != NULL) {
        *responseSize = size;
    }
    return buffer;
}


static char *bsFetchFile(const BSFetchRequest *request, size_t *responseSize, bool write)
{
    /* An HTTP(S) URL */
    if (bsUrlIsURL(request->url)) {
        return bsFetchHTTP(request, responseSize, NULL);
    }

    /* A file write */
    if (request->body != NULL) {
        if (!write) {
            return NULL;
        }
        FILE *file = fopen(request->url, "wb");
        if (file == NULL) {
            return NULL;
        }
        size_t written = fwrite(request->body, 1, request->bodySize, file);
        bool failed = (written != request->bodySize);
        failed = (fclose(file) != 0) || failed;
        if (failed) {
            return NULL; /* GCOV_EXCL_LINE */
        }
        if (responseSize != NULL) {
            *responseSize = 2;
        }
        return bsStrdup("{}");
    }

    return bsFileRead(request->url, responseSize);
}


char *bsFetchReadWrite(const BSFetchRequest *request, size_t *responseSize, void *data)
{
    return bsFetchFile(request, responseSize, true);
}


char *bsFetchReadOnly(const BSFetchRequest *request, size_t *responseSize, void *data)
{
    return bsFetchFile(request, responseSize, false);
}
