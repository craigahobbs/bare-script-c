/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * BareScript runtime option function implementations
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "barescript/options.h"

#include "internal.h"

#ifdef BARESCRIPT_CURL
/* The redirect-protocol option's long form is deprecated in the header, and is the form used here */
#define CURL_DISABLE_DEPRECATION
#include <curl/curl.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <time.h>
#endif


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
    size_t root = (size != 0 && path[0] == '/') ? 1 : 0; /* an absolute path keeps its leading separator */
    size_t resultSize = root;
    if (root != 0) {
        result[0] = '/';
    }

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
            /* Pop the previous segment and its separator, unless it is itself ".." or the path escapes its root */
            size_t previous = resultSize;
            while (previous > root && result[previous - 1] != '/') {
                previous--;
            }
            if (previous == resultSize) {
                if (root != 0) {
                    continue;
                }
            } else if (resultSize - previous != 2 || memcmp(result + previous, "..", 2) != 0) {
                resultSize = previous > root ? previous - 1 : previous;
                continue;
            }
        }
        if (resultSize > root) {
            result[resultSize++] = '/';
        }
        memcpy(result + resultSize, path + begin, segmentSize);
        resultSize += segmentSize;
    }

    if (resultSize == 0) {
        result[resultSize++] = '.';
    }
    result[resultSize] = '\0';
    return result;
}


char *bsUrlFileRelative(const char *url, void *data)
{
    const char *file = data != NULL ? (const char *) data : "";

    /* An absolute URL or POSIX path resolves to itself */
    if (bsUrlIsURL(url) || url[0] == '/') {
        return bsStrdup(url);
    }

    /* Replace the file's last path segment. A URL result stands; a file system path is normalized. */
    const char *lastSlash = strrchr(file, '/');
    size_t prefixSize = lastSlash != NULL ? (size_t) (lastSlash - file) + 1 : 0;
    size_t urlSize = strlen(url);
    char *joined = bsAlloc(prefixSize + urlSize + 1);
    memcpy(joined, file, prefixSize);
    memcpy(joined + prefixSize, url, urlSize + 1);
    if (lastSlash != NULL && bsUrlIsURL(file)) {
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
 * before it. curl_multi_poll, from libcurl 7.66, is optional; the rest are far older.
 */
typedef struct BSCurl {
    void *handle;
    CURLcode (*globalInit)(long);
    CURL *(*easyInit)(void);
    CURLcode (*easySetopt)(CURL *, CURLoption, ...);
    CURLcode (*easyGetinfo)(CURL *, CURLINFO, ...);
    void (*easyCleanup)(CURL *);
    CURLM *(*multiInit)(void);
    CURLMcode (*multiSetopt)(CURLM *, CURLMoption, ...);
    CURLMcode (*multiAddHandle)(CURLM *, CURL *);
    CURLMcode (*multiRemoveHandle)(CURLM *, CURL *);
    CURLMcode (*multiPerform)(CURLM *, int *);
    CURLMcode (*multiWait)(CURLM *, struct curl_waitfd *, unsigned int, int, int *);
    CURLMcode (*multiPoll)(CURLM *, struct curl_waitfd *, unsigned int, int, int *);
    CURLMsg *(*multiInfoRead)(CURLM *, int *);
    CURLMcode (*multiCleanup)(CURLM *);
    struct curl_slist *(*slistAppend)(struct curl_slist *, const char *);
    void (*slistFreeAll)(struct curl_slist *);
} BSCurl;

static BSCurl bsCurl;
static atomic_int bsCurlState;

/*
 * The thread's multi handle. It owns the connection pool - a connection stays open after its
 * transfer for the next fetch to the same host - and it runs a batch's transfers concurrently,
 * multiplexing those that share an HTTP/2 connection.
 */
static _Thread_local CURLM *bsCurlMulti;

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
        for (size_t ix = 0; handle == NULL && ix < BS_COUNT_OF(bsCurlLibraries); ix++) {
            handle = dlopen(bsCurlLibraries[ix], RTLD_LAZY | RTLD_LOCAL);
        }
        /* GCOV_EXCL_START - a libcurl that is missing or incomplete at runtime */
        if (handle != NULL &&
            (!bsCurlSymbol(handle, "curl_global_init", &bsCurl.globalInit) ||
             !bsCurlSymbol(handle, "curl_easy_init", &bsCurl.easyInit) ||
             !bsCurlSymbol(handle, "curl_easy_setopt", &bsCurl.easySetopt) ||
             !bsCurlSymbol(handle, "curl_easy_getinfo", &bsCurl.easyGetinfo) ||
             !bsCurlSymbol(handle, "curl_easy_cleanup", &bsCurl.easyCleanup) ||
             !bsCurlSymbol(handle, "curl_multi_init", &bsCurl.multiInit) ||
             !bsCurlSymbol(handle, "curl_multi_setopt", &bsCurl.multiSetopt) ||
             !bsCurlSymbol(handle, "curl_multi_add_handle", &bsCurl.multiAddHandle) ||
             !bsCurlSymbol(handle, "curl_multi_remove_handle", &bsCurl.multiRemoveHandle) ||
             !bsCurlSymbol(handle, "curl_multi_perform", &bsCurl.multiPerform) ||
             !bsCurlSymbol(handle, "curl_multi_wait", &bsCurl.multiWait) ||
             !bsCurlSymbol(handle, "curl_multi_info_read", &bsCurl.multiInfoRead) ||
             !bsCurlSymbol(handle, "curl_multi_cleanup", &bsCurl.multiCleanup) ||
             !bsCurlSymbol(handle, "curl_slist_append", &bsCurl.slistAppend) ||
             !bsCurlSymbol(handle, "curl_slist_free_all", &bsCurl.slistFreeAll))) {
            dlclose(handle);
            handle = NULL;
        }
        /* GCOV_EXCL_STOP */
        if (handle != NULL) {
            bsCurlSymbol(handle, "curl_multi_poll", &bsCurl.multiPoll);
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


/* True for an http or https URL - the schemes a browser's fetch accepts */
static bool bsUrlIsHTTP(const char *url)
{
    return strncmp(url, "http:", 5) == 0 || strncmp(url, "https:", 6) == 0;
}


/*
 * Wait for transfer activity. curl_multi_wait returns at once with no descriptors while a transfer
 * has no socket yet - a name resolving in a thread, a connection being waited for - so without
 * curl_multi_poll, which waits through that, the wait pauses briefly rather than spin.
 */
static CURLMcode bsCurlWait(const BSCurl *lib)
{
    CURLMcode code;
    if (lib->multiPoll != NULL) {
        code = lib->multiPoll(bsCurlMulti, NULL, 0, 1000, NULL);
    } else {
        /* GCOV_EXCL_START - libcurl before 7.66 */
        int descriptors = 0;
        code = lib->multiWait(bsCurlMulti, NULL, 0, 1000, &descriptors);
        if (code == CURLM_OK && descriptors == 0) {
            nanosleep(&(struct timespec) {0, 10000000}, NULL);
        }
        /* GCOV_EXCL_STOP */
    }
    return code;
}


/* One URL request's transfer */
typedef struct BSCurlTransfer {
    CURL *easy;
    struct curl_slist *headers;
    BSStringBuilder buffer;
    BSValue *response;
} BSCurlTransfer;


void bsFetchHTTP(const BSFetchRequest *requests, BSValue *responses, size_t count, void *data)
{
    /* A request that is not an http or https URL fails; the rest fetch together */
    size_t urlCount = 0;
    for (size_t ix = 0; ix < count; ix++) {
        urlCount += bsUrlIsHTTP(requests[ix].url);
    }
    const BSCurl *lib = urlCount != 0 ? bsCurlLoad() : NULL;
    if (lib == NULL) {
        return;
    }
    if (bsCurlMulti == NULL) {
        bsCurlMulti = lib->multiInit();
        /* A browser's limit on connections to one host, which keeps a batch from swamping a
           server's accept queue; an HTTP/2 connection multiplexes the batch regardless */
        lib->multiSetopt(bsCurlMulti, CURLMOPT_MAX_HOST_CONNECTIONS, 6L);
    }

    /* Start each URL request's transfer */
    BSCurlTransfer *transfers = bsAlloc(urlCount * sizeof(BSCurlTransfer));
    size_t transferCount = 0;
    for (size_t ix = 0; ix < count; ix++) {
        const BSFetchRequest *request = &requests[ix];
        if (!bsUrlIsHTTP(request->url)) {
            continue;
        }
        BSCurlTransfer *transfer = &transfers[transferCount++];
        *transfer = (BSCurlTransfer) {.easy = lib->easyInit(), .response = &responses[ix]};
        bsObjectIter(request->headers, bsCurlHeaderIter, &transfer->headers);

        CURL *easy = transfer->easy;
        lib->easySetopt(easy, CURLOPT_URL, request->url);
        lib->easySetopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
        /* A redirect to any other scheme fails - libcurl would follow one to FTP by default. The
           option's long form, deprecated for a string form since libcurl 7.85, is the one every
           libcurl has, whatever version the loader finds at runtime. */
        lib->easySetopt(easy, CURLOPT_REDIR_PROTOCOLS, (long) (CURLPROTO_HTTP | CURLPROTO_HTTPS));
        lib->easySetopt(easy, CURLOPT_WRITEFUNCTION, bsCurlWrite);
        lib->easySetopt(easy, CURLOPT_WRITEDATA, &transfer->buffer);
        lib->easySetopt(easy, CURLOPT_USERAGENT, "bare-script-c");
        lib->easySetopt(easy, CURLOPT_PRIVATE, transfer);
        /* No signals - a transfer would otherwise swap the process's SIGPIPE handler in and out,
           which threads fetching at once race on */
        lib->easySetopt(easy, CURLOPT_NOSIGNAL, 1L);
        /* libcurl negotiates HTTP/2 with a TLS server by default and multiplexes the transfers that
           share such a connection - so an HTTPS transfer waits for a connection already opening to
           its host, to learn whether it multiplexes, rather than open one of its own. A plain HTTP
           transfer cannot multiplex, and would only wait for that connection's first response. */
        if (strncmp(request->url, "https:", 6) == 0) {
            lib->easySetopt(easy, CURLOPT_PIPEWAIT, 1L);
        }
        if (request->body != NULL) {
            lib->easySetopt(easy, CURLOPT_POSTFIELDS, request->body);
            lib->easySetopt(easy, CURLOPT_POSTFIELDSIZE, (long) request->bodySize);
        }
        if (transfer->headers != NULL) {
            lib->easySetopt(easy, CURLOPT_HTTPHEADER, transfer->headers);
        }
        lib->multiAddHandle(bsCurlMulti, easy);
    }

    /* Run the transfers to completion - or until libcurl itself fails, leaving them unfinished */
    CURLMcode code;
    int running = 0;
    do {
        code = lib->multiPerform(bsCurlMulti, &running);
        if (code == CURLM_OK && running != 0) {
            code = bsCurlWait(lib);
        }
    } while (code == CURLM_OK && running != 0);

    /* A transfer that completed without error and with a 200 status is its request's response */
    int queued = 0;
    for (CURLMsg *message = lib->multiInfoRead(bsCurlMulti, &queued); message != NULL;
         message = lib->multiInfoRead(bsCurlMulti, &queued)) {
        char *transferData = NULL;
        lib->easyGetinfo(message->easy_handle, CURLINFO_PRIVATE, &transferData);
        BSCurlTransfer *transfer = (BSCurlTransfer *) transferData;
        long responseCode = 0;
        lib->easyGetinfo(message->easy_handle, CURLINFO_RESPONSE_CODE, &responseCode);
        if (message->data.result == CURLE_OK && responseCode == 200) {
            *transfer->response = bsSBToValue(&transfer->buffer);
        }
    }

    for (size_t ix = 0; ix < transferCount; ix++) {
        BSCurlTransfer *transfer = &transfers[ix];
        lib->multiRemoveHandle(bsCurlMulti, transfer->easy);
        lib->easyCleanup(transfer->easy);
        lib->slistFreeAll(transfer->headers);
        bsSBFree(&transfer->buffer);
    }
    free(transfers);
}


bool bsFetchHTTPAvailable(void)
{
    return bsCurlLoad() != NULL;
}


void bsFetchCleanup(void)
{
    if (bsCurlMulti != NULL) {
        bsCurl.multiCleanup(bsCurlMulti);
        bsCurlMulti = NULL;
    }
}

#else

void bsFetchHTTP(const BSFetchRequest *requests, BSValue *responses, size_t count, void *data)
{
    /* Every request fails, so the responses stay as they arrived - null values */
}


bool bsFetchHTTPAvailable(void)
{
    return false;
}


void bsFetchCleanup(void)
{
}

#endif


/*
 * File system fetch
 */


/* Read a file into a string value, or a null value if it cannot be read */
static BSValue bsFileRead(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return bsNull();
    }

    /*
     * Read into a string builder's spare room sized to the file, so the text becomes the string
     * uncopied and unpadded - one read short of the room shows the end
     */
    BSStringBuilder sb;
    bsSBInit(&sb);
    struct stat status;
    size_t reserve = fstat(fileno(file), &status) == 0 && status.st_size > 0 ? (size_t) status.st_size + 1 : 4096;
    size_t want;
    size_t read;
    do {
        bsSBReserve(&sb, reserve);
        want = sb.capacity - sb.size - 1;
        read = fread(sb.data + sb.size, 1, want, file);
        sb.size += read;
        reserve = 4096;
    } while (read == want);
    bool failed = (ferror(file) != 0);
    fclose(file);
    if (failed) {
        bsSBFree(&sb);   /* GCOV_EXCL_LINE */
        return bsNull(); /* GCOV_EXCL_LINE */
    }
    return bsSBToValue(&sb);
}


static bool bsFileWrite(const char *path, const char *body, size_t bodySize)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    bool written = (fwrite(body, 1, bodySize, file) == bodySize);
    return (fclose(file) == 0) && written;
}


static void bsFetchFile(const BSFetchRequest *requests, BSValue *responses, size_t count, bool write)
{
    /* The URL requests fetch together; the rest are file reads and, when allowed, file writes */
    bsFetchHTTP(requests, responses, count, NULL);
    for (size_t ix = 0; ix < count; ix++) {
        const BSFetchRequest *request = &requests[ix];
        if (bsUrlIsURL(request->url)) {
            continue;
        }
        if (request->body == NULL) {
            responses[ix] = bsFileRead(request->url);
        } else if (write && bsFileWrite(request->url, request->body, request->bodySize)) {
            responses[ix] = bsStringNew("{}");
        }
    }
}


void bsFetchReadWrite(const BSFetchRequest *requests, BSValue *responses, size_t count, void *data)
{
    bsFetchFile(requests, responses, count, true);
}


void bsFetchReadOnly(const BSFetchRequest *requests, BSValue *responses, size_t count, void *data)
{
    bsFetchFile(requests, responses, count, false);
}
