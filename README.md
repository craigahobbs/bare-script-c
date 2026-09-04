# bare-script-c

A pure-C implementation of the [BareScript](https://craigahobbs.github.io/bare-script/language/)
runtime: a shared library with the runtime's C API, and `bare`, its command-line interpreter.

BareScript is a small scripting language with a Pythonic syntax influenced by JavaScript, C, and
the Unix shell - functions, loops, arrays, objects, strings, regular expressions, JSON, and a
[library](https://craigahobbs.github.io/bare-script/library/) of built-in functions and included
scripts for markdown, schemas, and unit tests. It is the scripting language of
[MarkdownUp](https://craigahobbs.github.io/markdown-up/). A script that counts words:

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

The reference implementations are in [JavaScript](https://github.com/craigahobbs/bare-script) and
[Python](https://github.com/craigahobbs/bare-script-py). This one produces byte-identical output
across their 1,407-test suite, with 100% line coverage of its own C. It is also fast and small: on
real-world code it matches V8's bytecode interpreter and beats CPython, Lua, Ruby, and Perl, in a
469 KB library with no dependency beyond libm that starts in 3 ms and 3 MB. The measurements are
under [Performance](#performance).

```sh
make compile
./build/bare -c 'systemLog("Hello, World!")'
```


## Contents

- [Build](#build)
  - [Release Builds](#release-builds)
  - [Smaller Builds](#smaller-builds)
- [Command-Line Interface](#command-line-interface)
  - [System Includes](#system-includes)
  - [MarkdownUp Output](#markdownup-output)
  - [Static Analysis](#static-analysis)
- [Embedding the Runtime](#embedding-the-runtime)
  - [Native Functions](#native-functions)
  - [Values](#values)
  - [Fetching](#fetching)
  - [Threads](#threads)
- [Testing](#testing)
- [Performance](#performance)
  - [Cross-Language Benchmarks](#cross-language-benchmarks)
  - [Include Library Benchmarks](#include-library-benchmarks)
  - [Memory and Size](#memory-and-size)
- [Compatibility](#compatibility)
  - [jsonParse and regexNew messages](#jsonparse-and-regexnew-messages)
- [Design](#design)
- [License](#license)


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
variable names the includes to bundle and defines the macro for every other one:

```sh
make release INCLUDE="barescriptParser.bare barescriptLint.bare markdownUp.bare url.bare"
```

A compiled-out include keeps its registry entry and stub accessor, which return no model, so
`include <name.bare>` is served from the system include path when one is registered and fails
otherwise. There is no dependency tracking: an include that an included script itself includes has
to be listed with it - `markdownUp.bare` includes four scripts that include five more. The parser
and linter alone make a 286 KB release library, against 469 KB with all thirty-two. A change to
`INCLUDE` needs a `make clean` first, and the test suites need every include.


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

### System Includes

The BareScript include library - the language's standard library of scripts, `markdown.bare`,
`schema.bare`, `unittest.bare`, and the rest - is bundled into the library itself, so a system
include - `include <name.bare>` - resolves with no file system at all:

```sh
bare -c 'include <unittest.bare>
systemLog(systemType(unittestRunTest))'
```

A system include resolves in three steps: scripts registered with `bsSystemIncludeRegister`, then
the directories registered with `bsSystemIncludePath`, then the bundled library. The CLI adds every
directory in the colon-separated `BARESCRIPT_INCLUDE_PATH` environment variable to the search path,
so a script can run against an include library checkout instead of the bundled copy:

```sh
BARESCRIPT_INCLUDE_PATH=/path/to/bare-script/lib/include bare script.bare
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
    return bsNumber(values[0].u.number + values[1].u.number);
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

A `BSValue` is a 16-byte tagged struct passed by value. Null, boolean, number, and datetime values
are immediate and never allocate; string, array, object, function, and regex values point at a
reference-counted heap object.

```c
typedef struct BSValue {
    BSType type;
    union {
        bool boolean;
        double number;
        int64_t datetime;   /* milliseconds since the Unix epoch, UTC */
        BSString *string;
        BSArray *array;
        BSObject *object;
        BSFunction *function;
        BSRegex *regex;
        void *ref;          /* any reference-counted payload */
    } u;
} BSValue;
```

The reference counting rules are uniform:

- A function that **returns** a `BSValue` returns an *owned* reference; the caller releases it.
- A function that **takes** a `BSValue` takes a *borrowed* reference; it retains the value only if
  it keeps it beyond the call.
- Container accessors (`bsArrayGet`, `bsObjectGet`) return *borrowed* references.

Allocation failure is fatal: the runtime aborts rather than returning an out-of-memory error.

### Fetching

`options->fetchFn` serves `systemFetch` and file includes: a function that returns the response
as a `malloc`-allocated buffer, or `NULL` if the fetch failed.

```c
typedef struct BSFetchRequest {
    const char *url;
    const char *body;   /* NULL for a GET request */
    size_t bodySize;
    BSValue headers;    /* an object of string header values, or a null value */
} BSFetchRequest;

typedef char *(*BSFetchFn)(const BSFetchRequest *request, size_t *responseSize, void *data);
```

The function returns the response as a NUL-terminated, `malloc`-allocated buffer that the caller
frees, or `NULL` if the fetch failed. `responseSize`, when non-NULL, receives the response size, so
responses containing NUL bytes round-trip correctly.

Four options ship with the library:

| Function            | Behavior                                                    |
| ------------------- | ----------------------------------------------------------- |
| `bsFetchReadWrite`  | HTTP(S) URLs, otherwise read-write local file system access  |
| `bsFetchReadOnly`   | HTTP(S) URLs, otherwise read-only local file system access   |
| `bsFetchHTTP`       | HTTP(S) URLs only, via libcurl                               |
| *(none)*            | Leave `options->fetchFn` NULL to disable fetching entirely   |

`bsFetchHTTPAvailable` reports whether libcurl is available - compiled in, and loadable at runtime.
Without it the file system fetch functions still work and URL fetches fail.


### Threads

The runtime has no process-wide mutable state: every thread is an independent runtime, and
threads never contend. What that buys is confinement, and confinement is the rule: values,
scripts, expressions, and options belong to the thread that created them and cannot be handed to
another; pass text between threads instead and let the receiving thread parse it. A thread that used the runtime releases its
state before it exits with `bsParserCleanup`, `bsSystemIncludeClear`, `bsIncludeCleanup`,
`bsLibraryCleanup`, and last `bsValueCleanup`; a thread that exits without them leaks its copy.
The details are in [DESIGN.md](DESIGN.md#threads).

The public headers are in `include/barescript`: `value.h`, `parser.h`, `runtime.h`, `library.h`,
`json.h`, `regex.h`, `options.h`, `export.h` (the visibility macros), the generated
`includeSource.h`, and the `barescript.h` umbrella header.


## Testing

```sh
make test           # the C unit tests, at 100% line coverage
make test-include   # the BareScript include library's own 1,407-test suite
make test-language  # this project's own BareScript language tests
```

The include library suite is the reference implementations' own, vendored under
`lib/include/test`, and this runtime's output for it is identical to the JavaScript
implementation's byte for byte - a conformance check on the parser, the runtime, the library, the
regex engine, the linter, and the CLI at once. The one difference is the text of two `jsonParse`
debug messages, which follow the Python implementation - see [Compatibility](#compatibility).


## Performance

On real-world code this runtime matches V8's bytecode interpreter and beats CPython, Lua, Ruby,
and Perl, starts in 3 ms and 3 MB, and does it as a plain bytecode interpreter - no JIT - in
196 KB of code. Two suites back that up: `make perfx`, real-world-like applications ported to six
languages, and `make perf`, the include library's own suite, which the JavaScript and Python
implementations also run. Each table's last column scores a language against the best one: its
geometric mean across the tests, every test weighted equally, relative to the language with the
lowest mean, so 1.00x is the best language and 2x is twice its typical cost. Differences under
about 5% are within run-to-run drift.

### Cross-Language Benchmarks

`make perfx` runs an n-body simulation, web log analysis, a JSON pipeline, grid pathfinding, and
a CSV sales report, each ported to BareScript, JavaScript (V8 with and without its JIT), Python,
Lua, Ruby, and Perl, every port fed identical generated input and checked for the same result.

```sh
make perfx
make perfx PERFX_ARGS="--apps nbody --runs 5"   # options pass through; see perfx/perfx.py --help
make perfx-check                                # every port at a small size: do they all agree?
```

Application time on an Apple M3 Max (Node 26, Python 3.14, Lua 5.5, Ruby 2.6, Perl 5.34), best of
three, with the fastest per application in bold:

| Language                |       nbody | loganalyze |    jsonetl |   pathfind | salesreport | vs best |
| ----------------------- | ----------: | ---------: | ---------: | ---------: | ----------: | ------: |
| JavaScript (V8 JIT)     | **17.5 ms** | **504 ms** | **111 ms** | **131 ms** |  **144 ms** |   1.00x |
| JavaScript (V8 jitless) |      810 ms |     996 ms |     171 ms |     736 ms |      341 ms |   4.51x |
| BareScript              |      856 ms |     720 ms |     165 ms |     816 ms |      462 ms |   4.61x |
| Python                  |      923 ms |     1.01 s |     301 ms |     771 ms |      405 ms |   5.44x |
| Lua                     |      592 ms |     1.00 s |     1.28 s |     464 ms |      617 ms |   6.52x |
| Ruby                    |      1.18 s |     1.07 s |     512 ms |     1.08 s |      1.02 s |   8.26x |
| Perl                    |      2.18 s |     1.18 s |     4.47 s |     2.80 s |      996 ms |  17.70x |

Only V8's JIT is faster. BareScript leads every interpreter on the regex and JSON tests and trails
Lua by 1.5x to 1.8x where every field and element access is a library call. (Lua's JSON is a
pure-Lua codec; Perl's is its core `JSON::PP`.) Launching the empty program:

| Language                |    Wall | Peak RSS |
| ----------------------- | ------: | -------: |
| Lua                     |  2.7 ms |   1.7 MB |
| BareScript              |  3.3 ms |   3.1 MB |
| Perl                    |  4.6 ms |   4.3 MB |
| Python                  | 16.4 ms |  14.6 MB |
| JavaScript (V8 JIT)     | 26.9 ms |  38.2 MB |
| Ruby                    | 44.1 ms |  27.6 MB |

### Include Library Benchmarks

`make perf` runs the suite and merges the results of `../bare-script` and `../bare-script-py`
when present; `perf/test.c` is the native C baseline.

```sh
make perf
make perf TEST=mandelbrot PERF_RUNS=5
```

Time per 1000 runs on the same machine, best of five. `PyC` is the Python implementation with its
C extension; V8 and CPython 3.14 run the JavaScript and Python packages the library was ported
from (Python has no markdown ports). `testSuite` is the include library's test suite, which parses
about 2 MB of BareScript before its first test. Six of the nine tests are shown - `schemaParse`,
`qrcodeMatrix`, and `urlDecode` track `markdownParse`, `schemaValidate`, and `urlEncode` - and the
score covers all nine, each test's scale estimated from every implementation that runs it:

| Language         | mandelbrot |  mdElements |    mdParse | schValidate |  urlEncode | testSuite | vs best |
| ---------------- | ---------: | ----------: | ---------: | ----------: | ---------: | --------: | ------: |
| JavaScript (V8)  | **1.57 s** | **32.3 ms** | **618 ms** | **55.6 ms** | **2.2 ms** |           |   1.00x |
| BareScript (C)   |     19.0 s |      244 ms |     1.32 s |      164 ms |     5.0 ms | **220 s** |   3.02x |
| Python (CPython) |     45.9 s |             |            |      205 ms |    10.5 ms |           |   4.24x |
| BareScript (JS)  |      304 s |      721 ms |     2.85 s |      1.90 s |    49.5 ms |   1,830 s |  23.78x |
| BareScript (PyC) |      109 s |      664 ms |     7.15 s |      1.06 s |    57.0 ms |   7,500 s |  26.33x |
| BareScript (Py)  |    3,478 s |      5.25 s |     20.8 s |      14.4 s |     394 ms |  10,200 s | 188.28x |

The closest race is `markdownParse`, regular expressions against V8's JIT-compiled regex engine;
the widest is `markdownElements`, V8's inline caches allocating nested objects 7.6x faster. Native
C runs `mandelbrot` in 0.79 ms, 24x ahead.

### Memory and Size

Peak resident set of each perfx application, `empty` being the empty program, with the smallest
per column in bold:

| Language                |      empty |      nbody |   loganalyze |      jsonetl |    pathfind | salesreport | vs best |
| ----------------------- | ---------: | ---------: | -----------: | -----------: | ----------: | ----------: | ------: |
| Lua                     | **1.7 MB** | **1.8 MB** |     140.1 MB |     201.7 MB |     39.4 MB | **36.0 MB** |   1.00x |
| BareScript              |     3.1 MB |     3.3 MB |     136.8 MB | **139.3 MB** | **32.7 MB** |     69.9 MB |   1.24x |
| Perl                    |     4.3 MB |     6.8 MB |     136.5 MB |     219.8 MB |    113.3 MB |     86.8 MB |   2.03x |
| Python                  |    14.6 MB |    15.2 MB | **107.0 MB** |     162.2 MB |     56.3 MB |     73.6 MB |   2.25x |
| Ruby                    |    27.6 MB |    27.7 MB |     188.6 MB |     205.9 MB |     42.5 MB |    105.3 MB |   3.20x |
| JavaScript (V8 JIT)     |    38.2 MB |    44.7 MB |     363.1 MB |     159.7 MB |     69.9 MB |    198.6 MB |   4.73x |
| JavaScript (V8 jitless) |    37.7 MB |    40.7 MB |     357.0 MB |     181.3 MB |     86.0 MB |    196.0 MB |   4.89x |

BareScript is single-threaded, so its CPU time equals its wall time; V8 spends up to a third more
CPU than wall on background threads. Lua interns every short string, which halves its sales
report footprint, where the CSV fields repeat.

| This runtime                            |        |
| --------------------------------------- | -----: |
| Shared library                          | 469 KB |
| ... of which compressed include library | 205 KB |
| ... of which code                       | 196 KB |
| Empty script, resident set              | 3.1 MB |
| Empty script, peak footprint            | 2.4 MB |
| `make perf` test, peak                  |   6 MB |
| Include library test suite, peak        |  35 MB |

The empty script's floor is the process itself: libcurl loads on the first HTTP fetch, the parser
compiles from its model a statement at a time, and a script keeps its model only where lint or
coverage reads it. Memory figures are from `/usr/bin/time -l`.

## Compatibility

This runtime matches the JavaScript and Python implementations' observable behavior, including
error messages and their column numbers. Where the two reference implementations disagree with
each other, it follows the one shown in bold.

| Behavior                               | JavaScript        | Python          | This implementation |
| -------------------------------------- | ----------------- | --------------- | ------------------- |
| String length and indexing             | UTF-16 code units | code points     | **code points**     |
| `stringDecode` of a non-number element | a NUL character   | `null`          | **`null`**          |
| An unmatched capture group in a match  | omitted           | `null`          | **`null`**          |
| Zero-width `regexSplit` matches        | collapsed         | split at each   | **split at each**   |
| `String(1e-7)`                         | `1e-7`            | `1e-07`         | **`1e-7`**          |
| `String(-0)`                           | `0`               | `-0`            | **`0`**             |
| `-7 % 3`                               | `-1` (truncated)  | `2` (floored)   | **`-1`**            |
| Bitwise operators                      | 32-bit            | arbitrary width | **32-bit**          |
| A `regexNew` repeat count past 2^32    | accepted          | uncaught error  | **accepted**, saturating at 2^31 - 1 |
| `numberToString` past 2^53             | shortest round trip, exponential past 1e21 | the value's exact digits | **the value's exact digits** |

`objectKeys` returns keys in insertion order, matching both references for ordinary keys.
JavaScript additionally hoists integer-like keys to the front in ascending numeric order; this
implementation does not, matching Python.

One capability of the reference implementations is out of scope here:

- **Asynchronous functions.** Like the Python implementation, every function executes
  synchronously; the `async` keyword parses and is recorded in the model, but imposes no
  restriction. Scripts written for the JavaScript runtime run unchanged, and the linter's async
  checks - which need to know which functions are async - are skipped, as they are in Python.

An input nested more deeply than the evaluator's expression depth limit is reported as a parse
error rather than crashing; the JavaScript implementation overflows its own stack on the same
input.

### `jsonParse` and `regexNew` messages

A failed `jsonParse` or `regexNew` reports its own decoder's or compiler's message, so the two
reference implementations already differ from each other here. This implementation matches the
Python one: `jsonParse` reports CPython's `json` messages with their `line L column C (char N)`
position, and `regexNew` reports CPython's `re` messages with their `at position N`.

`jsonParse` matches exactly - every message, every position. So does `regexNew`, wherever the two
engines agree a pattern is invalid. They do not always agree, because BareScript specifies
JavaScript regular expressions and `re` is not one:

| Pattern         | JavaScript and this implementation | Python `re`                     |
| --------------- | ---------------------------------- | ------------------------------- |
| `(?i)`, `(?#c)` | `unknown extension`                | inline flags and comments       |
| `(?>a)`, `(?(1)a)` | `unknown extension`             | atomic groups and conditionals  |
| `a*+`           | `multiple repeat`                  | a possessive quantifier         |
| `[]`            | a set that never matches           | `unterminated character set`    |
| `[\k]`, `\cA`     | identity and control escapes       | `bad escape`                    |
| `\1(a)`         | a forward reference, matches empty | `invalid group reference`       |
| `(?<=a*)b`      | a variable-width lookbehind        | `look-behind requires fixed-width pattern` |

Where a pattern is invalid in both, the message and position match: 3918 of 4000 fuzzed patterns
agree with CPython character for character, and every one of the remaining 82 is a case from the
table above.


## Design

[DESIGN.md](DESIGN.md) describes how the runtime works - the value system, the register bytecode
and the parser that is itself a BareScript script, the bundled include library, the regular
expression engine, thread confinement - and how the release build and its training workload were
chosen.


## License

Licensed under the MIT License. See [LICENSE](LICENSE).
