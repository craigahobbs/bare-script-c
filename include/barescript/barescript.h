/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * BareScript - a simple, lightweight, and portable programming language
 *
 * This is the umbrella header for the BareScript C runtime library.
 */

#ifndef BARESCRIPT_H
#define BARESCRIPT_H

#include "json.h"
#include "library.h"
#include "options.h"
#include "parser.h"
#include "regex.h"
#include "runtime.h"
#include "value.h"

#ifdef __cplusplus
extern "C" {
#endif


/* The BareScript C library version */
#define BARESCRIPT_VERSION "1.0.0"

/* Get the BareScript C library version string */
const char *bsVersion(void);

/* The BareScript command-line interface entry point */
int bsMain(int argc, char **argv);


#ifdef __cplusplus
}
#endif

#endif
