/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * The unit test runner entry point
 */

#include "test.h"


int main(int argc, char **argv)
{
    return bsTestRun(argc > 1 ? argv[1] : NULL);
}
