# bare-script-c

A pure-C implementation of the [BareScript](https://craigahobbs.github.io/bare-script/language/)
runtime: a shared library with the runtime's C API, and `bare`, its command-line interpreter.

BareScript is a small scripting language with a Pythonic syntax influenced by JavaScript, C, and
the Unix shell - functions, loops, arrays, objects, strings, regular expressions, JSON, and a
[library](https://craigahobbs.github.io/bare-script/library/) of built-in functions and included
scripts for markdown, schemas, and unit tests. It is the scripting language of
[MarkdownUp](https://craigahobbs.github.io/markdown-up/).

```bare-script
# Compute a factorial
function factorial(n):
    return if(n <= 1, 1, n * factorial(n - 1))
endfunction

systemLog('factorial(10) = ' + factorial(10))
```

The reference implementations are in [JavaScript](https://github.com/craigahobbs/bare-script) and
[Python](https://github.com/craigahobbs/bare-script-py). This one produces byte-identical output
across their 1,407-test suite, with 100% line coverage of its own C. On real-world code it beats
V8's bytecode interpreter, CPython, Lua, Ruby, and Perl, as a plain bytecode interpreter with no
JIT. Measurements are on the
[BareScript (C) Performance](https://craigahobbs.github.io/bare-script-c/perf/).

```sh
make compile
./build/bare -c 'systemLog("Hello, World!")'
```


## Contents

- [Language](#language)
  - [System Includes](#system-includes)
- [Build](#build)
  - [Release Builds](#release-builds)
  - [Smaller Builds](#smaller-builds)
- [Command-Line Interface](#command-line-interface)
  - [MarkdownUp Output](#markdownup-output)
  - [Static Analysis](#static-analysis)
- [Embedding the Runtime](#embedding-the-runtime)
  - [Native Functions](#native-functions)
  - [Values](#values)
  - [Fetching](#fetching)
  - [Threads](#threads)
- [Testing](#testing)
- [Performance](#performance)
- [Compatibility](#compatibility)
- [Design](#design)
- [License](#license)


## Language

The [language documentation](https://craigahobbs.github.io/bare-script/language/) is the tour, and
the [library documentation](https://craigahobbs.github.io/bare-script/library/) the reference. For
example:

```bare-script
# Count the words of a text and report the most common ones
function wordCounts(text):
    counts = {}
    for word in regexSplit(regexNew('\\s+'), stringLower(text)):
        if word != '':
            objectSet(counts, word, objectGet(counts, word, 0) + 1)
        endif
    endfor
    return counts
endfunction

function compareCounts(a, b):
    return arrayGet(b, 1) - arrayGet(a, 1)
endfunction

counts = wordCounts('the quick brown fox jumps over the lazy dog the end')
entries = []
for word in objectKeys(counts):
    arrayPush(entries, [word, objectGet(counts, word)])
endfor
arraySort(entries, compareCounts)
for entry in arraySlice(entries, 0, 3):
    systemLog(arrayGet(entry, 0) + ': ' + arrayGet(entry, 1))
endfor
```

### System Includes

The BareScript include library - the language's standard library of scripts, `markdown.bare`,
`schema.bare`, `unittest.bare`, and the rest - is bundled into the library itself, so a system
include - `include <name.bare>` - resolves with no file system at all:

```bare-script
include <unittest.bare>

systemLog(systemType(unittestRunTest))
```

A system include is served from the bundled library and nowhere else. A user include -
`include 'name.bare'` - is fetched through the options' fetch function, relative to the including
script.


## Build

The project builds with any C11 compiler and GNU Make, and has no required dependencies beyond
libm. If libcurl's headers are present, HTTP support is compiled in automatically; the library
itself is loaded on the first HTTP fetch, so a script that never fetches a URL never pays for it.

```sh
make compile        # build build/libbarescript.{so,dylib}, build/libbarescript.a, and build/bare
make test           # build and run the unit tests
make release        # profile-guided optimization build in build/release
make install        # build the release and install it to $(PREFIX), default /usr/local
make clean          # remove the build directory
make                # show every target
```

### Release Builds

`make release` is a three-stage profile-guided build: compile instrumented, run a training
workload, then recompile with the profile and link-time optimization. It is about 1.5x faster
than the default `-O2` build and takes about ten seconds; `make install` installs it. The release
static library keeps the profile but not link-time optimization, so it is an archive of ordinary
object files that any linker consumes. How the flags and the training workload were chosen is in
[DESIGN.md](DESIGN.md#the-release-build).

```sh
make release
./build/release/bare script.bare
```

### Smaller Builds

Any include but `barescriptParser.bare` and `barescriptLint.bare` - the runtime parses with those -
can be compiled out to make the library smaller. Defining `NO_BARESCRIPT_INCLUDE_<NAME>`, the file
name without `.bare` in upper case, leaves that include's model out; the Makefile's `INCLUDE`
variable names the includes to bundle, `INCLUDE_EXCLUDE` the ones to leave out, and either defines
the macro for every include not bundled (the parser and linter are bundled either way):

```sh
make release INCLUDE="markdownUp.bare url.bare"
make release INCLUDE_EXCLUDE="qrcode.bare draw.bare"
```

A compiled-out include keeps its registry entry and stub accessor, which return no model, so
`include <name.bare>` fails. There is no dependency tracking: an include that an included script
itself includes has to be listed with it - `markdownUp.bare` includes four scripts that include five
more - and an excluded include is missing from every bundled include that includes it. The parser
and linter alone make a 321 KB release library, against 438 KB with all thirty-two. A change to
`INCLUDE` or `INCLUDE_EXCLUDE` needs a `make clean` first. Both are for a library built for an
application: the test suites and the release build's training need every include, so neither is
for a test or release build of this project.


## Command-Line Interface

```
usage: bare [-h] [-c CODE] [-d] [-l | -m] [-s] [-x] [-v VAR EXPR] [--version] [file ...]

The BareScript command-line interface

positional arguments:
  file            files to process

options:
  -h, --help      show this help message and exit
  -c, --code      execute the BareScript code
  -d, --debug     enable debug mode
  -l, --html      run with MarkdownUp HTML output
  -m, --markdown  run with MarkdownUp text output
  -s, --static    perform static analysis
  -x, --staticx   perform static analysis with execution
  -v, --var       set a global variable to an expression value
  --version       show the version and exit
```

Files and `-c` scripts execute in order, sharing one set of global variables. The process exit
status is the script's return value when it is an integer from 0 to 255, and otherwise 1 if the
return value is truthy.

```sh
bare script.bare
bare -c 'systemLog("Hello, World!")'
bare -v vName "'World'" script.bare
```

### MarkdownUp Output

`-m` runs the script with [MarkdownUp](https://craigahobbs.github.io/markdown-up/) text output
and `-l` with HTML output, wrapping it in the bundled `markdownUp.bare` include.

```sh
bare -m -c "markdownPrint('# Heading')"
bare -l doc.bare > doc.html
```

### Static Analysis

`-s` parses and lints without executing; `-x` executes first, so the linter sees the globals the
script defined. Both run barescriptLint.bare, the same linter the JavaScript and Python
implementations use.

```sh
bare -s script.bare
bare -x script.bare
```


## Embedding the Runtime

```c
#include <stdio.h>
#include <string.h>

#include "barescript/barescript.h"


int main(void)
{
    const char *text = "return arrayJoin(['Hello', 'World'], ', ')";

    BSParserError error;
    memset(&error, 0, sizeof(error));
    BSScript *script = bsParseScript(text, strlen(text), 1, "hello.bare", &error);
    if (script == NULL) {
        fputs(bsStringData(error.message), stderr);
        bsParserErrorFree(&error);
        return 1;
    }

    BSOptions *options = bsOptionsNew();
    options->logFn = bsLogStdout;
    options->fetchFn = bsFetchReadWrite;

    BSValue result = bsExecuteScript(script, options);
    if (bsErrorGet(options) != NULL) {
        fprintf(stderr, "%s\n", bsErrorGet(options));
    } else {
        printf("%s\n", bsStringData(result));
    }

    bsRelease(result);
    bsOptionsFree(options);
    bsScriptRelease(script);
    bsParserCleanup();
    bsIncludeCleanup();
    bsLibraryCleanup();
    bsValueCleanup();
    return 0;
}
```

```sh
cc -Iinclude example.c -Lbuild -lbarescript -o example
```

### Native Functions

A C function becomes a script function through `bsFunctionNew`, stored in the options' globals
object. Its arguments are validated against a model with the same coercion and range rules the
built-in library uses:

```c
static const BSArgModel addArgs[] = {
    {"a", BS_ARG_NUMBER, 0, 0, 0, 0, 0},
    {"b", BS_ARG_NUMBER, 0, 0, 0, 0, 0}
};

static BSValue add(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(addArgs, 2, args, argCount, values, options)) {
        return bsNull();
    }
    return bsNumber(bsNumberOf(values[0]) + bsNumberOf(values[1]));
}
```

```c
bsObjectSet(options->globals, "add", bsFunctionNew("add", add, NULL, NULL));
```

```c
typedef BSValue (*BSFunctionFn)(const BSValue *args, size_t argCount, BSOptions *options, void *data);
```

- `args` is a **borrowed** array of `argCount` values - the interpreter's reads of the call's
  operand registers and constants, never an allocated array object, so a call costs nothing beyond
  the argument evaluation itself.
- The return value is an **owned** reference.
- `data` is the function's closure data, which script functions use to carry their definition and
  `systemPartial` uses to carry its bound arguments.
- A **runtime error** - which halts the script - is signalled with `bsErrorSet` (or
  `bsErrorSetStatement` for a located error), not through the return value.
- An **argument error** - which does not halt the script - is reported by `bsArgsValidate`, which
  applies the same coercion and range rules as the reference implementations'
  `value_args_validate` and records the message for the evaluator to log in debug mode. The
  function returns its documented error value.

### Values

A `BSValue` is one 64-bit word passed by value. A number is a double as itself; every other value
lives in the space of negative quiet NaNs, which no number uses: the top thirteen bits set, a
four-bit type tag, and a 47-bit payload - an immediate null or boolean, or a pointer to a
reference-counted heap object. Strings, arrays, objects, functions, regexes, and datetimes are the
heap types: a datetime is boxed because JavaScript's `Date` range, 8.64e15 milliseconds either side
of the epoch, needs more than a payload holds. Read a value through the accessors, never its bits:

```c
typedef struct BSValue {
    uint64_t bits;
} BSValue;

BSType bsValueType(BSValue value);            /* BS_NULL, BS_BOOLEAN, BS_NUMBER, BS_DATETIME, BS_STRING, ... */
bool bsIsNumber(BSValue value);               /* one compare */
bool bsIsType(BSValue value, BSType type);    /* one compare, for any type but BS_NUMBER */
double bsNumberOf(BSValue value);             /* the payloads, each for a value of its type */
bool bsBoolOf(BSValue value);
int64_t bsDatetimeOf(BSValue value);          /* milliseconds since the Unix epoch, UTC */
BSString *bsStringOf(BSValue value);          /* and bsArrayOf, bsObjectOf, bsFunctionOf, bsRegexOf */
```

A zero-initialized `BSValue` is the number zero, as a zeroed `double` is; a null value is
`bsNull()`. A number is never a NaN: BareScript has no NaN, so `bsNumber` makes a null of one, as
the runtime's arithmetic does of a non-finite result.


The reference counting rules are uniform:

- A function that **returns** a `BSValue` returns an *owned* reference; the caller releases it.
- A function that **takes** a `BSValue` takes a *borrowed* reference; it retains the value only if
  it keeps it beyond the call.
- Container accessors (`bsArrayGet`, `bsObjectGet`) return *borrowed* references.

Allocation failure is fatal: the runtime aborts rather than returning an out-of-memory error.

### Fetching

`options->fetchFn` serves `systemFetch` and file includes: a function that fetches a batch of
requests and sets each one's response.

```c
typedef struct BSFetchRequest {
    const char *url;
    const char *body;   /* NULL for a GET request */
    size_t bodySize;
    BSValue headers;    /* an object of string header values, or a null value */
} BSFetchRequest;

typedef void (*BSFetchFn)(const BSFetchRequest *requests, BSValue *responses, size_t count, void *data);
```

The function fetches `count` requests at once and sets each successful request's response to its
text, an owned string value that the caller releases. The responses arrive as null values, so a
failed request's response stays null. A `systemFetch` of an array arrives as one batch, so an
implementation can fetch the requests concurrently, and so do an include statement's includes,
which then execute in order; a single URL arrives as a batch of one.

Four options ship with the library:

| Function            | Behavior                                                    |
| ------------------- | ----------------------------------------------------------- |
| `bsFetchReadWrite`  | HTTP(S) URLs, otherwise read-write local file system access  |
| `bsFetchReadOnly`   | HTTP(S) URLs, otherwise read-only local file system access   |
| `bsFetchHTTP`       | HTTP(S) URLs only, via libcurl                               |
| *(none)*            | Leave `options->fetchFn` NULL to disable fetching entirely   |

`bsFetchHTTP` fetches a batch's requests concurrently over the calling thread's connection pool,
which keeps a connection open for the next fetch to the same host, opens at most six to a host as
a browser does, and multiplexes the requests to an HTTPS server that speaks HTTP/2 over one
connection. It fetches http and https URLs - the schemes a browser's fetch accepts - and no other,
redirects included. `bsLibraryCleanup` releases the pool with the thread's other library state.
`bsFetchHTTPAvailable` reports whether libcurl is available - compiled in, and loadable at runtime.
Without it the file system fetch functions still work and URL fetches fail.


### Threads

The runtime has no process-wide mutable state: every thread is an independent runtime, and
threads never contend. What that buys is confinement, and confinement is the rule: values,
scripts, expressions, and options belong to the thread that created them and cannot be handed to
another; pass text between threads instead and let the receiving thread parse it. A thread that used the runtime releases its
state before it exits with `bsParserCleanup`, `bsIncludeCleanup`, `bsLibraryCleanup`, and last
`bsValueCleanup`; a thread that exits without them leaks its copy.
The details are in [DESIGN.md](DESIGN.md#threads).

The public headers are in `include/barescript`: `value.h`, `parser.h`, `runtime.h`, `library.h`,
`json.h`, `regex.h`, `options.h`, `export.h` (the visibility macros), the generated
`includeSource.h`, and the `barescript.h` umbrella header.


## Testing

```sh
make test           # the C unit tests, at 100% line coverage
make test-include   # the BareScript include library's own 1,407-test suite
make test-language  # this project's own BareScript language tests
make test-static    # the BareScript (C) Performance MarkdownUp app
```

The include library suite is the reference implementations' own, vendored under
`lib/include/test`, and this runtime's output for it is identical to the JavaScript
implementation's byte for byte - a conformance check on the parser, the runtime, the library, the
regex engine, the linter, and the CLI at once. The one difference is the text of two `jsonParse`
debug messages, which report CPython's message text - see [Compatibility](#compatibility).


## Performance

On real-world code this runtime beats V8's bytecode interpreter, CPython, Lua, Ruby, and Perl, as a
plain bytecode interpreter with no JIT. Two suites back that up: `make perfx`, real-world-like
applications ported to six languages, and `make perf`, the include library's own suite, which the
JavaScript and Python implementations also run.

The numbers live on the
[BareScript (C) Performance](https://craigahobbs.github.io/bare-script-c/perf/), a MarkdownUp
application that reads `static/perf/data/` (`perf.csv`, `perfx.json`, `size.json`).

To re-measure and publish:

```sh
make perf-data PERF_RUNS=5
make gh-pages
```


## Compatibility

This implementation defines BareScript. Where a port or a document disagrees with it, this
runtime's behavior is the language's and the other is the bug. The ports - the
[JavaScript](https://github.com/craigahobbs/bare-script) and
[Python](https://github.com/craigahobbs/bare-script-py) implementations - aspire to behave
identically, within reason: each is built on a host language with its own strings, numbers, and
regular expressions, and matching this runtime exactly is not always worth what it costs there. A
difference that cannot change what a real script computes is not worth writing down; one visible
enough to surprise a script author belongs in that port's README. None is recorded today.

The definition, as far as a script needs it:

- **Strings** measure and index by Unicode code point.

- **Numbers** are IEEE 754 doubles: `%` truncates, so `-7 % 3` is `-1`, the bitwise operators are
  32-bit, and there is no NaN and no infinity - an operation that would produce one yields null.

- **`objectKeys`** returns keys in insertion order. Integer-like keys are not hoisted to the front,
  as they are on a JavaScript object.

- **Regular expressions** are standard JavaScript's: the syntax DESIGN.md's
  [Regular Expressions](DESIGN.md#regular-expressions) table lists, with JavaScript's matching
  semantics. Where JavaScript accepts a legacy form that can only be a mistake, correctness wins
  over its behavior: a backreference to a group the pattern never defines (`(a)\2`, an octal escape
  in JavaScript), an incomplete `\x4` or `\u12`, `\8` and `\9`, `\k<n>` in a pattern with no named
  groups, and a quantified lookaround are errors here, as they are in JavaScript's unicode mode. A
  pattern may have at most 127 capture groups.

- **Whitespace** - the regex `\s` class, `stringTrim`, and the space around a parsed number - is
  JavaScript's WhiteSpace and LineTerminator sets: the ASCII spaces, U+00A0, U+1680, U+2000-U+200A,
  U+2028, U+2029, U+202F, U+205F, U+3000, and U+FEFF. The regex `\w`, `\d`, and `\b` are ASCII; `.`
  and the multi-line anchors know every line terminator - LF, CR, U+2028, U+2029 - and `$` alone is
  the end of the string.

- **Case** is Unicode 16.0's. `stringUpper` and `stringLower` apply the full case mapping: ß
  upper-cases to `SS`, the ligatures expand, and a capital sigma that ends a word lower-cases to the
  final sigma. The regex `i` flag folds as JavaScript does: a code point matches the ones sharing
  its simple upper case, but never across the ASCII boundary, and never through an expanding upper
  case.

- **Every function is synchronous.** The `async` keyword parses and is recorded in the model, but
  imposes no restriction, so a script written for an asynchronous runtime runs unchanged; the
  linter's async checks, which need to know which functions are async, are skipped.

- **`jsonParse` and `regexNew`** report their own decoder's and compiler's message: CPython's
  `json` messages with their `line L column C (char N)` position, and CPython's `re` messages with
  their `at position N`. A pattern that `re` reads differently - BareScript's regular expressions
  are JavaScript's, and `re`'s are not - is described by the nearest `re` message.


## Design

[DESIGN.md](DESIGN.md) describes how the runtime works - the value system, the register bytecode
and the parser that is itself a BareScript script, the bundled include library, the regular
expression engine, thread confinement - and how the release build and its training workload were
chosen.


## License

Licensed under the MIT License. See [LICENSE](LICENSE).
