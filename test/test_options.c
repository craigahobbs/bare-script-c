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
}


TEST(options_log_stdout)
{
    /* The stdout log function writes a line - exercised for coverage */
    bsLogStdout("test-log-line", NULL);
    ASSERT_TRUE(true);
}


TEST(options_fetch_file)
{
    const char *path = bsTestTempFile("read.txt", "file contents");

    BSFetchRequest request;
    memset(&request, 0, sizeof(request));
    request.url = path;
    request.headers = bsNull();

    size_t size = 0;
    char *text = bsFetchReadOnly(&request, &size, NULL);
    ASSERT_NOT_NULL(text);
    ASSERT_STR_EQ(text, "file contents");
    ASSERT_INT_EQ(size, 13);
    free(text);

    /* Read without a size output */
    text = bsFetchReadWrite(&request, NULL, NULL);
    ASSERT_NOT_NULL(text);
    free(text);

    /* A missing file */
    request.url = "no-such-file-xyz";
    ASSERT_NULL(bsFetchReadOnly(&request, &size, NULL));
    ASSERT_NULL(bsFetchReadWrite(&request, &size, NULL));

    /* A large file exercises the read buffer growth */
    BSStringBuilder sb;
    bsSBInit(&sb);
    for (int ix = 0; ix < 10000; ix++) {
        bsSBAppendString(&sb, "0123456789");
    }
    BSValue big = bsSBToValue(&sb);
    const char *bigPath = bsTestTempFile("big.txt", bsStringData(big));
    request.url = bigPath;
    text = bsFetchReadOnly(&request, &size, NULL);
    ASSERT_NOT_NULL(text);
    ASSERT_INT_EQ(size, 100000);
    free(text);
    bsRelease(big);
}


TEST(options_fetch_file_write)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/write.txt", bsTestTempDir());

    BSFetchRequest request;
    memset(&request, 0, sizeof(request));
    request.url = path;
    request.body = "written";
    request.bodySize = 7;
    request.headers = bsNull();

    size_t size = 0;
    char *result = bsFetchReadWrite(&request, &size, NULL);
    ASSERT_NOT_NULL(result);
    ASSERT_STR_EQ(result, "{}");
    ASSERT_INT_EQ(size, 2);
    free(result);

    /* Write without a size output */
    result = bsFetchReadWrite(&request, NULL, NULL);
    ASSERT_NOT_NULL(result);
    free(result);

    /* The read-only fetch function refuses writes */
    ASSERT_NULL(bsFetchReadOnly(&request, &size, NULL));

    /* An unwritable path */
    request.url = "no-such-directory-xyz/write.txt";
    ASSERT_NULL(bsFetchReadWrite(&request, &size, NULL));

    /* Read back what was written */
    memset(&request, 0, sizeof(request));
    request.url = path;
    request.headers = bsNull();
    char *text = bsFetchReadOnly(&request, &size, NULL);
    ASSERT_NOT_NULL(text);
    ASSERT_STR_EQ(text, "written");
    free(text);
}


TEST(options_fetch_http)
{
    /* A non-URL request is not an HTTP fetch */
    BSFetchRequest request;
    memset(&request, 0, sizeof(request));
    request.url = "not-a-url";
    request.headers = bsNull();
    ASSERT_NULL(bsFetchHTTP(&request, NULL, NULL));

    /* An unreachable host fails rather than hanging */
    request.url = "http://127.0.0.1:1/nope";
    ASSERT_NULL(bsFetchHTTP(&request, NULL, NULL));
    ASSERT_NULL(bsFetchReadOnly(&request, NULL, NULL));

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
 * A minimal single-request HTTP server, for exercising the libcurl fetch function
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>


/* Serve one HTTP request on a fresh port in a child process; returns the port, or zero on failure */
static int bsTestHTTPServe(pid_t *child, const char *status, const char *body)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        return 0; /* GCOV_EXCL_LINE */
    }
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    address.sin_port = 0;
    /* GCOV_EXCL_START */
    if (bind(listener, (struct sockaddr *) &address, sizeof(address)) != 0 || listen(listener, 1) != 0) {
        close(listener);
        return 0;
    }
    /* GCOV_EXCL_STOP */

    socklen_t addressSize = sizeof(address);
    getsockname(listener, (struct sockaddr *) &address, &addressSize);
    int port = ntohs(address.sin_port);

    pid_t pid = fork();
    /* GCOV_EXCL_START */
    if (pid < 0) {
        close(listener);
        return 0;
    }
    /* GCOV_EXCL_STOP */
    if (pid == 0) {
        /* The child serves one request and exits without running the test framework's cleanup */
        int connection = accept(listener, NULL, NULL);
        if (connection >= 0) {
            char request[4096];
            ssize_t read = recv(connection, request, sizeof(request) - 1, 0);
            (void) read;
            char response[8192];
            int size = snprintf(response, sizeof(response),
                                "HTTP/1.1 %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                                status, strlen(body), body);
            ssize_t written = send(connection, response, (size_t) size, 0);
            (void) written;
            close(connection);
        }
        close(listener);
        _exit(0);
    }

    close(listener);
    *child = pid;
    return port;
}


static void bsTestHTTPWait(pid_t child)
{
    int status = 0;
    waitpid(child, &status, 0);
}


TEST(options_fetch_http_get)
{
    if (!bsFetchHTTPAvailable()) {
        return; /* GCOV_EXCL_LINE */
    }

    pid_t child = 0;
    int port = bsTestHTTPServe(&child, "200 OK", "hello from http");
    ASSERT_TRUE(port != 0);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/x", port);
    BSFetchRequest request;
    memset(&request, 0, sizeof(request));
    request.url = url;
    request.headers = bsNull();
    size_t size = 0;
    char *text = bsFetchHTTP(&request, &size, NULL);
    bsTestHTTPWait(child);
    ASSERT_NOT_NULL(text);
    ASSERT_STR_EQ(text, "hello from http");
    ASSERT_INT_EQ(size, 15);
    free(text);
}


TEST(options_fetch_http_post)
{
    if (!bsFetchHTTPAvailable()) {
        return; /* GCOV_EXCL_LINE */
    }

    pid_t child = 0;
    int port = bsTestHTTPServe(&child, "200 OK", "posted");
    ASSERT_TRUE(port != 0);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/x", port);
    BSValue headers = bsObjectNew();
    bsObjectSet(headers, "X-Test", bsStringNew("value"));
    BSFetchRequest request;
    memset(&request, 0, sizeof(request));
    request.url = url;
    request.body = "body text";
    request.bodySize = 9;
    request.headers = headers;
    char *text = bsFetchReadOnly(&request, NULL, NULL);
    bsTestHTTPWait(child);
    ASSERT_NOT_NULL(text);
    ASSERT_STR_EQ(text, "posted");
    free(text);
    bsRelease(headers);
}


TEST(options_fetch_http_empty_and_error)
{
    if (!bsFetchHTTPAvailable()) {
        return; /* GCOV_EXCL_LINE */
    }

    /* An empty response body */
    pid_t child = 0;
    int port = bsTestHTTPServe(&child, "200 OK", "");
    ASSERT_TRUE(port != 0);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/x", port);
    BSFetchRequest request;
    memset(&request, 0, sizeof(request));
    request.url = url;
    request.headers = bsNull();
    size_t size = 1;
    char *text = bsFetchHTTP(&request, &size, NULL);
    bsTestHTTPWait(child);
    ASSERT_NOT_NULL(text);
    ASSERT_STR_EQ(text, "");
    ASSERT_INT_EQ(size, 0);
    free(text);

    /* A non-200 status is a failed fetch */
    port = bsTestHTTPServe(&child, "404 Not Found", "missing");
    ASSERT_TRUE(port != 0);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/x", port);
    request.url = url;
    ASSERT_NULL(bsFetchHTTP(&request, NULL, NULL));
    bsTestHTTPWait(child);
}


TEST(options_fetch_http_large)
{
    if (!bsFetchHTTPAvailable()) {
        return; /* GCOV_EXCL_LINE */
    }

    /* A response larger than the initial buffer exercises the buffer growth */
    BSStringBuilder sb;
    bsSBInit(&sb);
    for (int ix = 0; ix < 600; ix++) {
        bsSBAppendString(&sb, "0123456789");
    }
    BSValue body = bsSBToValue(&sb);

    pid_t child = 0;
    int port = bsTestHTTPServe(&child, "200 OK", bsStringData(body));
    ASSERT_TRUE(port != 0);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/x", port);
    BSFetchRequest request;
    memset(&request, 0, sizeof(request));
    request.url = url;
    request.headers = bsNull();
    size_t size = 0;
    char *text = bsFetchHTTP(&request, &size, NULL);
    bsTestHTTPWait(child);
    ASSERT_NOT_NULL(text);
    ASSERT_INT_EQ(size, 6000);
    free(text);
    bsRelease(body);
}
