/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * BareScript - a simple, lightweight, and portable programming language
 *
 * This is the umbrella header for the BareScript C runtime library.
 *
 * Threads: the runtime keeps its state - the value free lists, the intern table, the compiled
 * parser and include caches - in thread-local storage, so each thread is an independent runtime
 * and threads never contend. Values, scripts, expressions, and options belong to the thread that
 * created them and must not be passed to another; pass text and let the receiving thread parse it.
 * A thread that used the runtime releases its state before it exits with bsParserCleanup,
 * bsIncludeCleanup, bsLibraryCleanup, and last bsValueCleanup.
 */

#ifndef BARESCRIPT_H
#define BARESCRIPT_H

#include "includeSource.h"
#include "json.h"
#include "library.h"
#include "options.h"
#include "parser.h"
#include "regex.h"
#include "runtime.h"
#include "value.h"
#include "export.h"

#ifdef __cplusplus
extern "C" {
#endif

BS_VISIBILITY_BEGIN

/* The BareScript C library version */
#define BARESCRIPT_VERSION "1.0.0"

/* Get the BareScript C library version string */
const char *bsVersion(void);

/* The BareScript command-line interface entry point. Linked with the CLI, not the dylib. */
int bsMain(int argc, char **argv);

BS_VISIBILITY_END

#ifdef __cplusplus
}
#endif

#endif
