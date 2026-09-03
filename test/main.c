/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The unit test runner entry point
 */

#include <stdio.h>

#include "test.h"


static void usage(FILE *stream)
{
    fprintf(stream,
            "usage: bare-test [-q] [filter]\n"
            "\n"
            "  filter       run only the tests whose name contains the substring\n"
            "  -q, --quiet  print a dot per test instead of a line, failure details at the end\n"
            "  -h, --help   show this help\n");
}


int main(int argc, char **argv)
{
    const char *filter = NULL;
    bool quiet = false;
    for (int ix = 1; ix < argc; ix++) {
        const char *arg = argv[ix];
        if (strcmp(arg, "-q") == 0 || strcmp(arg, "--quiet") == 0) {
            quiet = true;
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "bare-test: unknown option \"%s\"\n", arg);
            usage(stderr);
            return 2;
        } else if (filter == NULL) {
            filter = arg;
        } else {
            fprintf(stderr, "bare-test: only one filter is allowed\n");
            usage(stderr);
            return 2;
        }
    }
    return bsTestRun(filter, quiet);
}
