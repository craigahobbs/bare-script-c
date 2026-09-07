/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The runtime option function unit tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test.h"

#include "../src/internal.h"


/* Resolve a URL relative to a file, returning the result as a value */
static BSValue bsTestUrl(const char *file, const char *url)
{
    char fileBuffer[256];
    void *fileData = NULL;
    if (file != NULL) {
        snprintf(fileBuffer, sizeof(fileBuffer), "%s", file);
        fileData = fileBuffer;
    }
    char *resolved = bsUrlFileRelative(url, fileData);
    BSValue result = bsStringNew(resolved);
    free(resolved);
    return result;
}


TEST(options_url_is_url)
{
    ASSERT_TRUE(bsUrlIsURL("http://example.com"));
    ASSERT_TRUE(bsUrlIsURL("https://example.com"));
    ASSERT_TRUE(bsUrlIsURL("file:/x"));
    ASSERT_FALSE(bsUrlIsURL("HTTP://example.com"));
    ASSERT_FALSE(bsUrlIsURL("/abs/path"));
    ASSERT_FALSE(bsUrlIsURL("rel/path"));
    ASSERT_FALSE(bsUrlIsURL(":x"));
    ASSERT_FALSE(bsUrlIsURL("abc"));
}


TEST(options_url_file_relative)
{
    /* Absolute URLs and paths resolve to themselves */
    ASSERT_VALUE_STRING(bsTestUrl("dir/script.bare", "http://example.com/x"), "http://example.com/x");
    ASSERT_VALUE_STRING(bsTestUrl("dir/script.bare", "/abs/path"), "/abs/path");

    /* Relative to a URL */
    ASSERT_VALUE_STRING(bsTestUrl("http://example.com/a/b.bare", "c.bare"), "http://example.com/a/c.bare");
    ASSERT_VALUE_STRING(bsTestUrl("http:noslash", "c.bare"), "http:noslashc.bare");

    /* Relative to a file system path */
    ASSERT_VALUE_STRING(bsTestUrl("dir/script.bare", "other.bare"), "dir/other.bare");
    ASSERT_VALUE_STRING(bsTestUrl("dir/sub/script.bare", "../other.bare"), "dir/other.bare");
    ASSERT_VALUE_STRING(bsTestUrl("script.bare", "other.bare"), "other.bare");
    ASSERT_VALUE_STRING(bsTestUrl("dir/script.bare", "./other.bare"), "dir/other.bare");
    ASSERT_VALUE_STRING(bsTestUrl("dir/script.bare", "sub//other.bare"), "dir/sub/other.bare");
    ASSERT_VALUE_STRING(bsTestUrl("dir/script.bare", "../../other.bare"), "../other.bare");
    ASSERT_VALUE_STRING(bsTestUrl("/a/b/script.bare", "../c.bare"), "/a/c.bare");
    ASSERT_VALUE_STRING(bsTestUrl("/script.bare", "../../c.bare"), "/c.bare");
    ASSERT_VALUE_STRING(bsTestUrl("dir/script.bare", "."), "dir");
    ASSERT_VALUE_STRING(bsTestUrl("script.bare", "."), ".");
    ASSERT_VALUE_STRING(bsTestUrl(NULL, "other.bare"), "other.bare");
    ASSERT_VALUE_STRING(bsTestUrl("dir/script.bare", "a/../../b"), "b");
    ASSERT_VALUE_STRING(bsTestUrl("dir/script.bare", "../a/../../b"), "../b");
    ASSERT_VALUE_STRING(bsTestUrl("dir/script.bare", "sub/.."), "dir");
    ASSERT_VALUE_STRING(bsTestUrl("script.bare", "../a/../.."), "../..");
    ASSERT_VALUE_STRING(bsTestUrl("script.bare", "../a/../../b"), "../../b");
    ASSERT_VALUE_STRING(bsTestUrl("script.bare", "../../b"), "../../b");
    ASSERT_VALUE_STRING(bsTestUrl("/a/script.bare", "../../b"), "/b");
    ASSERT_VALUE_STRING(bsTestUrl("/script.bare", ".."), "/");
}


TEST(options_log_stdout)
{
    /* The stdout log function writes a line - exercised for coverage */
    bsLogStdout("test-log-line", NULL);
}


/* Fetch one request through a fetch function - the response arrives as a null value, as the fetch
   function signature requires - and return the owned response */
static BSValue bsTestFetch(BSFetchFn fetchFn, const BSFetchRequest *request)
{
    BSValue response = bsNull();
    fetchFn(request, &response, 1, NULL);
    return response;
}


TEST(options_fetch_file)
{
    const char *path = bsTestTempFile("read.txt", "file contents");

    BSFetchRequest request = {.url = path, .headers = bsNull()};
    ASSERT_VALUE(bsTestFetch(bsFetchReadOnly, &request), "\"file contents\"");
    ASSERT_VALUE(bsTestFetch(bsFetchReadWrite, &request), "\"file contents\"");

    /* A missing file */
    request.url = "no-such-file-xyz";
    ASSERT_VALUE(bsTestFetch(bsFetchReadOnly, &request), "null");
    ASSERT_VALUE(bsTestFetch(bsFetchReadWrite, &request), "null");

    /* A large file exercises the read buffer growth */
    BSValue big = bsTestRepeat(NULL, "0123456789", 10000, NULL);
    const char *bigPath = bsTestTempFile("big.txt", bsStringData(big));
    request.url = bigPath;
    ASSERT_VALUE_STRING(bsTestFetch(bsFetchReadOnly, &request), bsStringData(big));
    bsRelease(big);
}


TEST(options_fetch_file_write)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/write.txt", bsTestTempDir());

    BSFetchRequest request = {.url = path, .body = "written", .bodySize = 7, .headers = bsNull()};
    ASSERT_VALUE(bsTestFetch(bsFetchReadWrite, &request), "\"{}\"");

    /* The read-only fetch function refuses writes */
    ASSERT_VALUE(bsTestFetch(bsFetchReadOnly, &request), "null");

    /* An unwritable path */
    request.url = "no-such-directory-xyz/write.txt";
    ASSERT_VALUE(bsTestFetch(bsFetchReadWrite, &request), "null");

    /* Read back what was written */
    request = (BSFetchRequest) {.url = path, .headers = bsNull()};
    ASSERT_VALUE(bsTestFetch(bsFetchReadOnly, &request), "\"written\"");
}


TEST(options_fetch_http)
{
    /* A non-URL request is not an HTTP fetch */
    BSFetchRequest request = {.url = "not-a-url", .headers = bsNull()};
    ASSERT_VALUE(bsTestFetch(bsFetchHTTP, &request), "null");

    /* An unreachable host fails rather than hanging */
    request.url = "http://127.0.0.1:1/nope";
    ASSERT_VALUE(bsTestFetch(bsFetchHTTP, &request), "null");
    ASSERT_VALUE(bsTestFetch(bsFetchReadOnly, &request), "null");

    /* An unreachable HTTPS host, whose transfer would wait to multiplex, fails the same way */
    request.url = "https://127.0.0.1:1/nope";
    ASSERT_VALUE(bsTestFetch(bsFetchHTTP, &request), "null");

    /* Only http and https URLs are fetched - as with a browser's fetch, a file URL fails */
    char fileUrl[600];
    snprintf(fileUrl, sizeof(fileUrl), "file://%s", bsTestTempFile("scheme.txt", "local"));
    request.url = fileUrl;
    ASSERT_VALUE(bsTestFetch(bsFetchHTTP, &request), "null");
    ASSERT_VALUE(bsTestFetch(bsFetchReadOnly, &request), "null");

    /* HTTP availability reflects the build configuration */
#ifdef BARESCRIPT_CURL
    ASSERT_TRUE(bsFetchHTTPAvailable());
#else
    ASSERT_FALSE(bsFetchHTTPAvailable());
#endif
}


TEST(options_version)
{
    ASSERT_STR_EQ(bsVersion(), BARESCRIPT_VERSION);
}


/*
 * A minimal HTTP server, for exercising the libcurl fetch function
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>


/*
 * Serve "count" HTTP requests from a child process on a fresh port, answering each with the status
 * (which may carry further header lines) and body. Each request arrives on a connection of its
 * own, closed after its response - or, with "keepAlive", all arrive on the first connection, and
 * the listener closes behind it so that a client opening another connection is refused rather than
 * left waiting. Returns the port, or zero on failure.
 */
static int bsTestHTTPServe(pid_t *child, const char *status, const char *body, int count, bool keepAlive)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        return 0;
    }
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    address.sin_port = 0;
    if (bind(listener, (struct sockaddr *) &address, sizeof(address)) != 0 || listen(listener, 16) != 0) {
        close(listener);
        return 0;
    }

    socklen_t addressSize = sizeof(address);
    getsockname(listener, (struct sockaddr *) &address, &addressSize);
    int port = ntohs(address.sin_port);

    pid_t pid = fork();
    if (pid < 0) {
        close(listener);
        return 0;
    }
    if (pid == 0) {
        /* The child serves and exits without running the test framework's cleanup - and dies rather
           than wait forever for a request that never comes */
        alarm(10);
        const char *closeHeader = keepAlive ? "" : "Connection: close\r\n";
        size_t responseCapacity = strlen(status) + strlen(body) + strlen(closeHeader) + 64;
        char *response = malloc(responseCapacity);
        int size = snprintf(response, responseCapacity, "HTTP/1.1 %s\r\nContent-Length: %zu\r\n%s\r\n%s",
                            status, strlen(body), closeHeader, body);
        int connection = -1;
        for (int served = 0; served < count; served++) {
            if (connection < 0) {
                connection = accept(listener, NULL, NULL);
                if (connection < 0) {
                    break;
                }
                if (keepAlive) {
                    close(listener);
                    listener = -1;
                }
            }
            char request[4096];
            if (recv(connection, request, sizeof(request) - 1, 0) <= 0) {
                break;
            }
            ssize_t written = send(connection, response, (size_t) size, 0);
            (void) written;
            if (!keepAlive) {
                close(connection);
                connection = -1;
            }
        }
        if (connection >= 0) {
            close(connection);
        }
        if (listener >= 0) {
            close(listener);
        }
        _exit(0);
    }

    close(listener);
    *child = pid;
    return port;
}


/* Reap the server child, which must have exited on its own rather than been killed by its alarm */
static void bsTestHTTPWait(pid_t child)
{
    int status = 0;
    waitpid(child, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));
}


/*
 * Serve one HTTP response from a child process and point "request" at it. Returns false when the
 * library has no HTTP support, in which case the test has nothing to check.
 */
static bool bsTestHTTPRequest(pid_t *child, BSFetchRequest *request, const char *status, const char *body)
{
    static char url[64];
    if (!bsFetchHTTPAvailable()) {
        return false;
    }
    int port = bsTestHTTPServe(child, status, body, 1, false);
    ASSERT_TRUE(port != 0);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/x", port);
    *request = (BSFetchRequest) {.url = url, .headers = bsNull()};
    return true;
}


TEST(options_fetch_http_get)
{
    pid_t child = 0;
    BSFetchRequest request;
    if (!bsTestHTTPRequest(&child, &request, "200 OK", "hello from http")) {
        return;
    }
    BSValue response = bsTestFetch(bsFetchHTTP, &request);
    bsTestHTTPWait(child);
    ASSERT_VALUE(response, "\"hello from http\"");
}


TEST(options_fetch_http_post)
{
    pid_t child = 0;
    BSFetchRequest request;
    if (!bsTestHTTPRequest(&child, &request, "200 OK", "posted")) {
        return;
    }
    BSValue headers = bsObjectNew();
    bsObjectSet(headers, "X-Test", bsStringNew("value"));
    request.body = "body text";
    request.bodySize = 9;
    request.headers = headers;
    BSValue response = bsTestFetch(bsFetchReadOnly, &request);
    bsTestHTTPWait(child);
    ASSERT_VALUE(response, "\"posted\"");
    bsRelease(headers);
}


TEST(options_fetch_http_empty_and_error)
{
    /* An empty response body */
    pid_t child = 0;
    BSFetchRequest request;
    if (!bsTestHTTPRequest(&child, &request, "200 OK", "")) {
        return;
    }
    BSValue response = bsTestFetch(bsFetchHTTP, &request);
    bsTestHTTPWait(child);
    ASSERT_VALUE(response, "\"\"");

    /* A non-200 status is a failed fetch */
    bsTestHTTPRequest(&child, &request, "404 Not Found", "missing");
    response = bsTestFetch(bsFetchHTTP, &request);
    bsTestHTTPWait(child);
    ASSERT_VALUE(response, "null");
}


TEST(options_fetch_http_large)
{
    /* A response larger than the initial buffer exercises the buffer growth */
    BSValue body = bsTestRepeat(NULL, "0123456789", 600, NULL);

    pid_t child = 0;
    BSFetchRequest request;
    if (!bsTestHTTPRequest(&child, &request, "200 OK", bsStringData(body))) {
        bsRelease(body);
        return;
    }
    BSValue response = bsTestFetch(bsFetchHTTP, &request);
    bsTestHTTPWait(child);
    ASSERT_VALUE_STRING(response, bsStringData(body));
    bsRelease(body);
}


TEST(options_fetch_http_redirect)
{
    if (!bsFetchHTTPAvailable()) {
        return;
    }

    /* A redirect is followed */
    pid_t childTarget = 0;
    int portTarget = bsTestHTTPServe(&childTarget, "200 OK", "landed", 1, false);
    ASSERT_TRUE(portTarget != 0);
    char status[128];
    snprintf(status, sizeof(status), "302 Found\r\nLocation: http://127.0.0.1:%d/target", portTarget);
    pid_t childRedirect = 0;
    int portRedirect = bsTestHTTPServe(&childRedirect, status, "", 1, false);
    ASSERT_TRUE(portRedirect != 0);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/x", portRedirect);
    BSFetchRequest request = {.url = url, .headers = bsNull()};
    BSValue response = bsTestFetch(bsFetchHTTP, &request);
    bsTestHTTPWait(childRedirect);
    bsTestHTTPWait(childTarget);
    ASSERT_VALUE(response, "\"landed\"");

    /* A redirect to another scheme is not followed - here to a file URL, which libcurl can read */
    char fileStatus[700];
    snprintf(fileStatus, sizeof(fileStatus), "302 Found\r\nLocation: file://%s",
             bsTestTempFile("redirect.txt", "local"));
    pid_t childFile = 0;
    int portFile = bsTestHTTPServe(&childFile, fileStatus, "", 1, false);
    ASSERT_TRUE(portFile != 0);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/x", portFile);
    response = bsTestFetch(bsFetchHTTP, &request);
    bsTestHTTPWait(childFile);
    ASSERT_VALUE(response, "null");
}


TEST(options_fetch_http_batch)
{
    if (!bsFetchHTTPAvailable()) {
        return;
    }

    /* Three requests to one server and one to another, among a file read and a missing file - each
       response lands on its request */
    pid_t childA = 0;
    pid_t childB = 0;
    int portA = bsTestHTTPServe(&childA, "200 OK", "from a", 3, false);
    ASSERT_TRUE(portA != 0);
    int portB = bsTestHTTPServe(&childB, "200 OK", "from b", 1, false);
    ASSERT_TRUE(portB != 0);
    char urlA[64];
    char urlB[64];
    snprintf(urlA, sizeof(urlA), "http://127.0.0.1:%d/a", portA);
    snprintf(urlB, sizeof(urlB), "http://127.0.0.1:%d/b", portB);
    const char *path = bsTestTempFile("batch.txt", "file contents");
    BSFetchRequest requests[] = {
        {.url = urlA, .headers = bsNull()},
        {.url = path, .headers = bsNull()},
        {.url = urlB, .headers = bsNull()},
        {.url = urlA, .headers = bsNull()},
        {.url = "no-such-file-xyz", .headers = bsNull()},
        {.url = urlA, .headers = bsNull()}
    };
    BSValue responses[6] = {0};
    bsFetchReadOnly(requests, responses, 6, NULL);
    bsTestHTTPWait(childA);
    bsTestHTTPWait(childB);
    const char *expected[] = {"\"from a\"", "\"file contents\"", "\"from b\"", "\"from a\"", "null", "\"from a\""};
    for (size_t ix = 0; ix < 6; ix++) {
        ASSERT_VALUE(responses[ix], expected[ix]);
    }
}


TEST(options_fetch_http_reuse)
{
    if (!bsFetchHTTPAvailable()) {
        return;
    }

    /* The server accepts one connection and serves two requests on it, refusing any other - so the
       second fetch succeeds only over the first's pooled connection */
    pid_t child = 0;
    int port = bsTestHTTPServe(&child, "200 OK", "again", 2, true);
    ASSERT_TRUE(port != 0);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/x", port);
    BSFetchRequest request = {.url = url, .headers = bsNull()};
    ASSERT_VALUE(bsTestFetch(bsFetchHTTP, &request), "\"again\"");
    BSValue response = bsTestFetch(bsFetchHTTP, &request);
    /* Releasing the pool closes its connection, so a server still reading it exits either way */
    bsFetchCleanup();
    bsTestHTTPWait(child);
    ASSERT_VALUE(response, "\"again\"");

    /* Releasing an empty pool is a no-op */
    bsFetchCleanup();
}
