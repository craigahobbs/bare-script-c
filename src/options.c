/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * BareScript runtime option function implementations
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "barescript/barescript.h"
#include "barescript/options.h"

#include "internal.h"

#ifdef BARESCRIPT_CURL
#include <curl/curl.h>
#include <dlfcn.h>
#include <stdatomic.h>
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
                bool isParent = resultSize - previous == 2 && memcmp(result + previous, "..", 2) == 0;
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

    /* Replace the file's last path segment. A URL result stands; a file system path is normalized. */
    bool fileIsURL = file != NULL && bsUrlIsURL(file);
    const char *lastSlash = file != NULL ? strrchr(file, '/') : NULL;
    size_t prefixSize = lastSlash != NULL ? (size_t) (lastSlash - file) + 1 : (fileIsURL ? strlen(file) : 0);
    size_t urlSize = strlen(url);
    char *joined = bsAlloc(prefixSize + urlSize + 1);
    if (prefixSize != 0) {
        memcpy(joined, file, prefixSize);
    }
    memcpy(joined + prefixSize, url, urlSize + 1);
    if (fileIsURL) {
        return joined;
    }
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

/*
 * libcurl is loaded on the first HTTP fetch rather than linked. Linking it made every process map
 * libcurl and its dependency tree at startup - several megabytes of resident memory and a third of
 * an empty script's instructions - whether or not the script ever fetched a URL.
 *
 * This is the runtime's one piece of process-wide state. libcurl is loaded and globally
 * initialized once, by the first thread to fetch, behind an atomic state: 0 before, 1 while that
 * thread loads, 2 after. Every other thread waits for 2 and reads the entry points published
 * before it.
 */
typedef struct BSCurl {
    void *handle;
    CURLcode (*globalInit)(long);
    CURL *(*easyInit)(void);
    CURLcode (*easySetopt)(CURL *, CURLoption, ...);
    CURLcode (*easyPerform)(CURL *);
    CURLcode (*easyGetinfo)(CURL *, CURLINFO, ...);
    void (*easyCleanup)(CURL *);
    struct curl_slist *(*slistAppend)(struct curl_slist *, const char *);
    void (*slistFreeAll)(struct curl_slist *);
} BSCurl;

static BSCurl bsCurl;
static atomic_int bsCurlState;

#ifdef __APPLE__
static const char *const bsCurlLibraries[] = {"libcurl.4.dylib", "libcurl.dylib"};
#else
static const char *const bsCurlLibraries[] = {"libcurl.so.4", "libcurl.so"};
#endif


/* Resolve a libcurl symbol into a function pointer (POSIX guarantees the two are convertible) */
static bool bsCurlSymbol(void *handle, const char *name, void *function)
{
    void *symbol = dlsym(handle, name);
    memcpy(function, &symbol, sizeof(symbol));
    return symbol != NULL;
}


/* The loaded libcurl entry points, or NULL if libcurl is not available at runtime */
static const BSCurl *bsCurlLoad(void)
{
    int state = 0;
    if (atomic_compare_exchange_strong(&bsCurlState, &state, 1)) {
        void *handle = NULL;
        for (size_t ix = 0; handle == NULL && ix < sizeof(bsCurlLibraries) / sizeof(bsCurlLibraries[0]); ix++) {
            handle = dlopen(bsCurlLibraries[ix], RTLD_LAZY | RTLD_LOCAL);
        }
        /* GCOV_EXCL_START - a libcurl that is missing or incomplete at runtime */
        if (handle != NULL &&
            (!bsCurlSymbol(handle, "curl_global_init", &bsCurl.globalInit) ||
             !bsCurlSymbol(handle, "curl_easy_init", &bsCurl.easyInit) ||
             !bsCurlSymbol(handle, "curl_easy_setopt", &bsCurl.easySetopt) ||
             !bsCurlSymbol(handle, "curl_easy_perform", &bsCurl.easyPerform) ||
             !bsCurlSymbol(handle, "curl_easy_getinfo", &bsCurl.easyGetinfo) ||
             !bsCurlSymbol(handle, "curl_easy_cleanup", &bsCurl.easyCleanup) ||
             !bsCurlSymbol(handle, "curl_slist_append", &bsCurl.slistAppend) ||
             !bsCurlSymbol(handle, "curl_slist_free_all", &bsCurl.slistFreeAll))) {
            dlclose(handle);
            handle = NULL;
        }
        /* GCOV_EXCL_STOP */
        if (handle != NULL) {
            /* curl_global_init is not thread-safe before libcurl 7.84 - so it runs here, once */
            bsCurl.globalInit(CURL_GLOBAL_DEFAULT);
            bsCurl.handle = handle;
        }
        atomic_store_explicit(&bsCurlState, 2, memory_order_release);
    }

    /* Another thread may be loading - wait for it */
    while (atomic_load_explicit(&bsCurlState, memory_order_acquire) != 2) {
    }
    return bsCurl.handle != NULL ? &bsCurl : NULL;
}


static size_t bsCurlWrite(char *data, size_t size, size_t count, void *userData)
{
    bsSBAppend(userData, data, size * count);
    return size * count;
}


static bool bsCurlHeaderIter(BSValue key, BSValue item, void *data)
{
    struct curl_slist **headers = data;
    BSValue header = bsStringNewFormat("%s: %s", bsStringData(key), bsStringData(item));
    *headers = bsCurl.slistAppend(*headers, bsStringData(header));
    bsRelease(header);
    return true;
}


char *bsFetchHTTP(const BSFetchRequest *request, size_t *responseSize, void *data)
{
    if (!bsUrlIsURL(request->url)) {
        return NULL;
    }

    const BSCurl *lib = bsCurlLoad();
    CURL *curl = lib != NULL ? lib->easyInit() : NULL;
    /* GCOV_EXCL_START */
    if (curl == NULL) {
        return NULL;
    }
    /* GCOV_EXCL_STOP */

    BSStringBuilder buffer;
    bsSBInit(&buffer);
    struct curl_slist *headers = NULL;
    bsObjectIter(request->headers, bsCurlHeaderIter, &headers);

    lib->easySetopt(curl, CURLOPT_URL, request->url);
    lib->easySetopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    lib->easySetopt(curl, CURLOPT_WRITEFUNCTION, bsCurlWrite);
    lib->easySetopt(curl, CURLOPT_WRITEDATA, &buffer);
    lib->easySetopt(curl, CURLOPT_USERAGENT, "bare-script-c");
    if (request->body != NULL) {
        lib->easySetopt(curl, CURLOPT_POSTFIELDS, request->body);
        lib->easySetopt(curl, CURLOPT_POSTFIELDSIZE, (long) request->bodySize);
    }
    if (headers != NULL) {
        lib->easySetopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    CURLcode status = lib->easyPerform(curl);
    long responseCode = 0;
    lib->easyGetinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
    lib->slistFreeAll(headers);
    lib->easyCleanup(curl);

    if (status != CURLE_OK || (responseCode != 0 && responseCode != 200)) {
        bsSBFree(&buffer);
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
    return bsCurlLoad() != NULL;
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
