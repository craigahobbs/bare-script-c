# bare-script-c

A pure-C implementation of the [BareScript](https://craigahobbs.github.io/bare-script/language/)
runtime. The build produces a shared library with the runtime exports and the `bare` command-line
interface.

BareScript is a simple, lightweight, and portable programming language with a Pythonic syntax
influenced by JavaScript, C, and the Unix shell.

This is the fastest BareScript runtime by a wide margin, and a fast interpreter by any standard. A
469 KB shared library with no dependency beyond libm runs the reference test suite 6x faster than
the JavaScript implementation on V8 and 27x faster than the Python one, keeps pace with V8's own
bytecode interpreter, and runs interpreted BareScript faster than CPython runs the equivalent
Python. The measurements are under [Performance](#performance).

```sh
make compile
./build/bare -c 'systemLog("Hello, World!")'
```


## Contents

- [Build](#build)
  - [Release Builds](#release-builds)
- [Command-Line Interface](#command-line-interface)
  - [System Includes](#system-includes)
  - [MarkdownUp Output](#markdownup-output)
  - [Static Analysis](#static-analysis)
- [Embedding the Runtime](#embedding-the-runtime)
- [Design](#design)
  - [The Value System](#the-value-system)
  - [The Function Format](#the-function-format)
  - [The Fetch Function Format](#the-fetch-function-format)
  - [The Parser and Linter](#the-parser-and-linter)
  - [The Bundled Include Library](#the-bundled-include-library)
  - [JSON](#json)
  - [Regular Expressions](#regular-expressions)
- [Testing](#testing)
  - [The Include Library Test Suite](#the-include-library-test-suite)
- [Performance](#performance)
  - [Against Other Languages](#against-other-languages)
- [Compatibility](#compatibility)
  - [jsonParse and regexNew messages](#jsonparse-and-regexnew-messages)
- [License](#license)


## Build

The project builds with any C11 compiler and GNU Make, and has no required dependencies beyond
libm. If libcurl's headers are present, HTTP support is compiled in automatically; the library
itself is loaded on the first HTTP fetch, so a script that never fetches a URL never pays for it.

```sh
make                # show the available targets
make compile        # build build/libbarescript.{so,dylib}, build/libbarescript.a, and build/bare
make test           # build and run the unit tests
make cover          # run the unit tests and report line coverage; fails under 100%
make test-include   # run the BareScript include library test suite
make test-language  # run this project's own BareScript language tests
make perf           # run the performance suite against the release build
make release        # profile-guided optimization build in build/release
make includes       # regenerate the bundled include library source
make clean          # remove the build directory
make install        # build the release and install it to $(PREFIX), default /usr/local
```

### Release Builds

`make release` is a three-stage profile-guided build: compile instrumented, run a training
workload, then recompile with the profile and link-time optimization. It is worth about 1.35x over
the default `-O2` build on the performance suite and 1.3x on the include library test suite, and
takes about ten seconds. `make install` installs it.

```sh
make release
./build/release/bare script.bare
```

The flags were chosen by measurement, over the performance suite, relative to a plain `-O2` build:

| Flags                    | Mean  | Notes                                              |
| ------------------------ | -----:| -------------------------------------------------- |
| `-O2`                    | 1.00x | the default build                                   |
| `-O3`                    | 1.00x | no measurable difference from `-O2` on its own      |
| **`-Os`**                | 1.07x | **slower** - every test, no exceptions              |
| `-Oz`                    | 1.43x | much slower                                         |
| `-O2 -flto`              | 0.93x |                                                     |
| PGO, `-O3`               | 0.87x |                                                     |
| PGO, `-Os -flto`         | 0.87x | PGO does not rescue `-Os`                           |
| PGO, `-O3 -flto`         | 0.82x |                                                     |
| **PGO, `-O2 -flto`**     | 0.82x | **the release build** - `-O3`'s speed, 10% less code |

`-Os` is the interesting one, since trading code size for instruction cache residency often wins
in an interpreter. The dispatch loop and the value operations are small and hot enough that the
inlining `-O2` and `-O3` do is worth more than the 11% of text section `-Os` gives back.

Link-time optimization is the single largest flag-level win - 7% on its own, and still 6% on top
of PGO - because the value system is small functions across translation unit boundaries:
`bsRetain`, `bsRelease`, and the object treap's comparisons are called from everywhere and can
only be inlined across the library at link time.

Under PGO and LTO, `-O2` emits about 10% less text than `-O3` at the same speed. Also
measured on the same suite, and not used: `-fno-stack-protector` (3% less text, 1.6% fewer instructions, no measurable time - not
worth a mitigation), `-fomit-frame-pointer`, `-flto=thin`, `-mcpu=native`, and disabling the
code generator's tail merging to keep every threaded-dispatch jump distinct (8% more text, no
gain) all land within build-to-build noise, which is about 1% between two builds of the same
configuration. Dead stripping has nothing left to strip after LTO with hidden visibility, and
context-sensitive PGO is not available: Apple's linker does not instrument under LTO.

The release static library keeps the profile but drops link-time optimization, so it stays an
archive of ordinary object files rather than one of compiler intermediate code, which not every
consumer's linker can read.

A PGO profile is only as good as the workload that produces it - the optimizer lays out branches
and inlines call sites in the proportion the training run exercises them.

| Program | Role |
| ------- | ---- |
| `perf/test.bare` | the official suite - the benchmark itself |
| `lib/include/test/runTests.bare` | the include library test suite: parses about 2 MB of BareScript from source and runs every include library function. This is the path `bare script.bare` takes; the suite never does, since it loads bundled models. |

An earlier mix added a synthetic source-parse script, this project's language tests, and a
static-analysis run instead of the test suite. Measured against a rebuilt identical configuration,
none of the three moved either workload beyond build-to-build noise, while training on the test
suite retires 3% fewer instructions on it and costs the performance suite nothing.

`make test` and `make cover` accept a `TEST` variable that filters test cases by name substring:

```sh
make test TEST=regex
```


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

The BareScript include library is bundled into the library itself, so a system include -
`include <name.bare>` - resolves with no file system at all:

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

`-m` runs the script with MarkdownUp text output and `-l` with HTML output, wrapping it in the
bundled `markdownUp.bare` include.

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
    return 0;
}
```

```sh
cc -Iinclude example.c -Lbuild -lbarescript -o example
```

The public headers are in `include/barescript`: `value.h`, `parser.h`, `runtime.h`, `library.h`,
`json.h`, `regex.h`, `options.h`, `export.h` (the visibility macros), the generated
`includeSource.h`, and the `barescript.h` umbrella header.


## Design

### The Value System

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

**Strings** are immutable, reference-counted UTF-8 buffers: a 32-byte header followed by the
NUL-terminated payload in the same allocation. They cache their code point length, so an
all-ASCII string - the common case - indexes by byte. Construction skips the UTF-8 walk when the
buffer has no high bit. Non-ASCII indexing keeps a cursor and, after a backward lookup, a sparse
stride-16 offset table. String library functions index by Unicode code point. Strings are the
runtime's most frequent allocation and most are short, so an allocation that fits one of four size
classes is rounded up and recycled through that class's free list.

**Arrays** are vectors of values with amortized growth.

**Objects** are key/value pairs in insertion order, with a sorted view for the operations defined
over sorted keys. Up to four pairs are stored in the 112-byte object itself; past that, pairs
live on a doubly-linked list of nodes, so objects iterate in insertion order (matching the
reference implementations, whose objects are JavaScript objects and Python dictionaries). Objects
of more than 32 keys index the list with an interned-pointer hash table for lookup, and the two
storage forms share the object's storage since an object is only ever one of them.

The sorted view is a *treap*: a binary search tree ordered by key that also satisfies a max-heap
property on a pseudo-random per-node priority, which keeps it balanced in expectation without the
bookkeeping of an AVL or red-black tree - BareScript code routinely inserts keys in sorted order,
the worst case for a plain binary search tree. It is built lazily, over the same nodes, the first
time an object past 32 keys is JSON-encoded, compared, or given a key that can only be matched
by content; an object whose keys are all interned never builds it otherwise. C-string keys and
compiled names of at most 64 bytes are interned, so a hit compares interned `BSString` pointers
instead of `memcmp` - and two distinct interned strings are known to differ without one. JSON
and computed keys reuse an interned name when it is already in the table and otherwise stay
ordinary strings, so untrusted unique keys cannot grow the table. The intern table is also
capped. Interned names on the compiled script skip hashing entirely. A hash-table miss is
definitive unless the object also has uninterned keys. The intern table holds one reference;
interned strings live until process exit.

Allocation failure is fatal: there is no useful way for a script runtime to continue without
memory, and threading an out-of-memory result through every value operation would obscure the code
for a case that cannot be tested.


### The Function Format

```c
typedef BSValue (*BSFunctionFn)(const BSValue *args, size_t argCount, BSOptions *options, void *data);
```

- `args` is a **borrowed** array of `argCount` values - a slice of the interpreter's value stack,
  never an allocated array object, so a call costs nothing beyond the argument evaluation itself.
- The return value is an **owned** reference.
- `data` is the function's closure data, which script functions use to carry their definition and
  `systemPartial` uses to carry its bound arguments.
- A **runtime error** - which halts the script - is signalled with `bsErrorSet` (or
  `bsErrorSetStatement` for a located error), not through the return value.
- An **argument error** - which does not halt the script - is reported by `bsArgsValidate`, which
  applies the same coercion and range rules as the reference implementations'
  `value_args_validate` and records the message for the evaluator to log in debug mode. The
  function returns its documented error value.

```c
static const BSArgModel arrayGetArgs[] = {
    {"array", BS_ARG_ARRAY, 0, 0, 0, 0, 0},
    {"index", BS_ARG_NUMBER, BS_ARG_INTEGER | BS_ARG_GTE, 0, 0, 0, 0}
};

static BSValue bsFnArrayGet(const BSValue *args, size_t argCount, BSOptions *options, void *data)
{
    BSValue values[2];
    if (!bsArgsValidate(arrayGetArgs, 2, args, argCount, values, options)) {
        return bsNull();
    }
    ...
}
```

The library's own functions open with a `BS_ARGS(model, failValue)` macro that expands to exactly
this prologue, sizing the model with `sizeof`. A few of the most-called functions - `arrayGet`,
`objectGet`, `mathAbs`, and their kin - also carry an *intrinsic* id, and the interpreter's call
path handles their happy-path argument shapes itself, without validation; any other shape falls
through to the function.


### The Fetch Function Format

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


### The Parser and Linter

BareScript is parsed by **barescriptParser.bare** and linted by **barescriptLint.bare** - the same
include library scripts the JavaScript and Python implementations use, running on this runtime.
The syntax accepted, the lowering of structured statements, and the exact text and column of every
error message are therefore shared with the reference implementations rather than reimplemented.

That is a bootstrap problem: the parser is a BareScript script, so parsing it would need a parser.
It is solved the way the reference implementations solve it - the bundled parser is stored as its
own parser-compiled JSON model, which loads with a JSON decode and no parser at all.

```
barescriptParser.bare (bundled JSON model)
        |  JSON decode
        v
  BareScript model  --.
        |             |  bsScriptFromModel
        v             v
   bytecode chunk  -> executed by the runtime, which is what parses your script
```

`model.c` compiles the model to bytecode. Jump labels become instruction indexes and
function-local names become slot indexes during emit - there is no executable expression tree. A slot holding the internal unset marker
falls through to the globals object, matching the reference behavior where an unassigned local
simply is not a key of the locals dictionary. Group nodes stay in the model (the parser and linter
observe them) and flatten only in the code stream.

The emitter tracks the value stack depth, so the interpreter allocates each chunk's stack once and
pushes without bounds checks, and it folds `jumpif (!expr)` into a jump-if-false. Every global
function call and global variable read compiles to a per-site cache that points at the globals
object's value slot for the name; the cache is re-resolved only when a key is added to or removed
from the globals object (its *structural generation*), so an assignment to a global never
invalidates other sites. Under GNU C the interpreter dispatches through a label table, one indirect
branch per opcode. A runtime error is detected at entry, after each call, and at the statements
that raise one; the statement's line number is found from a per-chunk table only when an error
message needs it. Bundled include scripts - the parser, the linter, and the library - are never
statement-counted or coverage-recorded, so their statement markers are stripped from the code
stream when the cached script is finished.

When `__barescriptCoverage` is enabled, each compiled script keeps a line-indexed array of
pointers into the coverage object's per-line counts, so a loop increments a number instead of
formatting a line key and searching the covered object on every statement.

`bsScriptToModel` and `bsExprToModel` return the model (with `scriptName` / `scriptLines` /
`system` overlaid), which is how the linter receives a script and how `barescriptEvaluateExpression`
works. A model is several times the size of its script text and only the linter and coverage
reporting read it, so a compiled script does not have to keep one: `bsScriptForgetModel` drops it,
and `bsScriptToModel` then re-parses the lines the script retains. The CLI forgets each script's
model unless static analysis was requested, and the runtime forgets an include's unless coverage
is recording - so the include library's test suite, which records coverage, keeps its models, while
a script that merely includes the library holds bytecode and source lines alone.


### The Bundled Include Library

The thirty-two scripts of the BareScript include library - `args.bare`, `markdown.bare`,
`schema.bare`, `unittest.bare`, `gzip.bare`, and the rest - are compiled to JSON
script models and embedded in the library. Including one costs a JSON decode rather than a run of
the parser.

The models are gzip-compressed at level 9 by `gzip.bare` (`gzipCompress` / `gzipUncompress`, byte
arrays in and out) and embedded as `unsigned char` arrays. That compresses about 601 KB of include
library source to about 201 KB of gzip. `bin/includeSource.bare` serializes the object and array
structure itself, in `objectKeys` (insertion) order, and delegates only leaf values to
`jsonStringify` so number formatting and string escaping stay exactly what the runtime produces.

`src/includeSource.c` and `include/barescript/includeSource.h` are generated and checked in, so a
fresh clone builds with no bootstrap. `make includes` regenerates them by running
`bin/includeSource.bare` - itself a BareScript program - under a CLI built from the *existing*
generated source, with `BARESCRIPT_INCLUDE_PATH` pointing at `lib/include` so `gzip.bare` is
available before it is bundled.

The generated header exports a stub accessor per include, returning its decoded JSON model:

```c
const char *bsIncludeSourceUnittest(void);      /* unittest.bare */
const char *bsIncludeSourceMarkdownUp(void);    /* markdownUp.bare */
/* ... one per bundled script ... */

extern const BSIncludeSourceFn bsIncludeSourceStubs[BS_INCLUDE_COUNT];   /* all of them, in order */
```

`runtime.h` adds `bsIncludeCount`, `bsIncludeName`, and `bsIncludeSource` for lookup by name. A model decodes
on first use and is cached, so a program that includes two of the thirty-two pays for two.


### JSON

`bsJSONEncode` writes values with object keys in sorted order, which the value system provides for
free. Datetime and function values encode as their `bsValueString` representation and regex values
as null, matching the reference implementations. `bsJSONDecode` parses into a `BSValue`; an
unpaired surrogate in a `\uXXXX` escape becomes U+FFFD so every decoded string is valid UTF-8.


### Regular Expressions

The engine compiles a pattern to a node tree and matches by backtracking with an explicit
continuation list. It implements the subset of JavaScript regular expression syntax that
BareScript's regex functions expose:

| Category   | Syntax                                                                    |
| ---------- | ------------------------------------------------------------------------- |
| Literals   | `.` `[...]` `[^...]` `( )` `(?: )` `(?<name> )` alternation               |
| Lookaround | `(?= )` `(?! )` `(?<= )` `(?<! )`                                          |
| Escapes    | `\d \D \w \W \s \S \b \B \n \r \t \f \v \0 \xHH \uHHHH \k<name>` `\1`-`\9` |
| Repeats    | `* + ? {n} {n,} {n,m}` and their lazy `?` forms                             |
| Anchors    | `^ $`                                                                      |
| Flags      | `i` (case-insensitive), `m` (multi-line), `s` (dot matches newline)         |

Matching is over Unicode code points, so match indexes agree with the string library's indexes.
ASCII subjects match the original bytes without widening to a `uint32_t` buffer. Five properties
keep it well-behaved on real input - which matters more here than in the reference implementations,
because the parser is itself regex-driven:

- A pattern whose every alternative begins with `^` only tries the search start position. Every
  pattern the parser uses is anchored this way, so this is the difference between a linear and a
  quadratic scan of each line it parses.
- An unanchored pattern computes the set of code points a match can begin with, and the search
  skips every position whose code point is not in it.

- A quantifier whose body matches exactly one code point - `\s*`, `[0-9]+`, `.*`, the overwhelming
  majority of real patterns - matches **iteratively**, so the C stack stays bounded on long
  subjects.
- Capture writes are recorded on an **undo trail**, so backtracking out of a lookaround restores
  state in time proportional to what changed rather than copying the capture array.
- Recursion depth and backtracking steps are **budgeted**, so a pathological pattern gives up
  rather than hanging the runtime or overflowing the stack.


## Testing

```sh
make test           # the C unit tests
make cover          # the same tests with line coverage; fails under 100%
make test-include   # the BareScript include library test suite
make test-language  # this project's own BareScript language tests
```

`make cover VERBOSE=1` lists every uncovered line.

The C unit tests self-register, so adding one is a single `TEST(name) { ... }` block. Coverage is
gathered with `gcov`/`llvm-cov` and summarized by `test/coverage.awk`, which honors
`GCOV_EXCL_LINE` and `GCOV_EXCL_START`/`GCOV_EXCL_STOP` markers - used only for out-of-memory
aborts, platform-specific fallbacks, and the checks that guard against a corrupted parser.

### The Include Library Test Suite

`make test-include` runs the BareScript include library's own test suite - vendored under
`lib/include/test` - as the JavaScript and Python implementations run it, in the same three parts:

| Target                    | What it runs                                                    |
| ------------------------- | --------------------------------------------------------------- |
| `test-include-run`        | 1407 tests, 18,015 statements, at 100% BareScript-level coverage |
| `test-include-markdownup` | the 22 `markdownUp.bare` tests                                   |
| `test-include-lint`       | static analysis of all 69 library and test scripts               |

All three produce output identical to the JavaScript implementation's, so a diff against
`bare-script` is a conformance check on the parser, the runtime, the library, the regex engine, the
linter, and the CLI at once. The one difference is the text of two `jsonParse` debug messages,
which this implementation reports the way the Python implementation does - see
[Compatibility](#compatibility).

`make test-language` runs this project's own suite, written against a small self-contained harness
so it runs unchanged on all three implementations.


## Performance

`make perf` runs the include library's performance suite - the same `perf/test.bare` the JavaScript
and Python implementations run - writes `build/perf.csv`, merges in the results from
`../bare-script` and `../bare-script-py` when they are present, and prints the same report they
print. `perf/test.c` is the native C baseline, the counterpart of their `test.js` and `test.py`.

It measures the **release** build, and builds it first if needed. The implementations it is
compared against are themselves optimized builds - Node ships as one, and CPython is built with
profile-guided and link-time optimization - so timing the development build here would understate
this runtime by about 1.35x against them.

```sh
make perf
make perf TEST=mandelbrot PERF_RUNS=5
```

Milliseconds per 1000 runs, best of two, on one machine - lower is better:

| Test             | BareScript (C) | BareScript (JS) | BareScript (PyC) | BareScript (Py) |
| ---------------- | --------------:| ---------------:| ----------------:| ---------------:|
| mandelbrot       |     **24,000** |         303,000 |          114,000 |       3,421,000 |
| markdownElements |        **392** |             740 |              665 |           5,227 |
| markdownParse    |      **2,696** |           2,968 |            7,236 |          20,952 |
| qrcodeMatrix     |      **1,833** |          12,800 |            9,033 |         124,800 |
| schemaParse      |        **152** |           1,188 |            1,256 |           9,044 |
| schemaValidate   |        **196** |           1,920 |            1,060 |          14,428 |
| urlDecode        |        **9.5** |              90 |            131.5 |           683.5 |
| urlEncode        |        **6.5** |            50.5 |             57.5 |             394 |

The C runtime is the fastest BareScript runtime on all eight tests. (`BareScript (PyC)` is the
Python implementation running its C extension for the runtime core, so it is not a pure-Python
baseline; `BareScript (Py)` is.) The closest race is `markdownParse`, almost entirely regular
expression work, against V8's JIT-compiled regex engine. That test parses each project's own
README, so its row moves when this file changes.

### Against Other Languages

Beating the other BareScript runtimes is table stakes. `make perf` also times each host language
doing the same work natively: `perf/test.c` for `mandelbrot`, and for the library tests the
JavaScript and Python packages the include library was ported from - `schema-markdown` and
`markdown-model` - JIT-compiled by V8 and interpreted by CPython 3.14. Milliseconds per 1000 runs
on the same machine, columns in speed order (the Python suite has no markdown tests):

| Test             | JavaScript (V8) | BareScript (C) | Python (CPython) |
| ---------------- | ---------------:| --------------:| ----------------:|
| mandelbrot       |           1,968 |         24,000 |           46,202 |
| markdownElements |            32.7 |            392 |                  |
| markdownParse    |             645 |          2,696 |                  |
| schemaParse      |            72.8 |            152 |            171.5 |
| schemaValidate   |            57.0 |            196 |            206.2 |
| urlDecode        |             5.0 |            9.5 |             11.9 |
| urlEncode        |             2.6 |            6.5 |             10.6 |

Every row reads the same way: V8's JIT, then this runtime, then CPython. An interpreted BareScript
program on this runtime runs the reference schema parser faster than CPython runs the pure-Python
package it was ported from - and does it through a parser that is itself written in BareScript.
Against the JIT it is 2x to 3.5x behind on the schema and URL tests and 4x behind on
`markdownParse`, which is almost entirely regular expression work against V8's compiled regex
engine. The exception is `markdownElements`, which builds nested objects and arrays as fast as
V8's hidden classes and inline caches can allocate them; there V8 is 12x ahead.

`mandelbrot` is the one test every language can run as the same code, so it can widen the field.
Milliseconds per run on the same machine - the Ruby and Perl rows are one-off ports of
`perf/test.c`'s function, the rest come from the perf suites:

| Runtime                                          | ms per run |
| ------------------------------------------------ | ----------:|
| C, clang `-O2` with LTO                          |       0.79 |
| JavaScript, Node 24 (V8 JIT)                     |       1.97 |
| **BareScript (C)**                               |     **24** |
| JavaScript, Node 24 `--jitless` (V8 interpreter) |         27 |
| Python 3.14 (CPython, PGO and LTO)               |         46 |
| Ruby 2.6 (no JIT)                                |         49 |
| Perl 5.34                                        |         85 |
| BareScript (PyC)                                 |        114 |
| BareScript (JS)                                  |        303 |
| BareScript (Py)                                  |      3,421 |

That is the neighborhood: 12x behind V8's JIT and 30x behind native C, level with V8's own
bytecode interpreter, twice as fast as CPython, and 5x to 140x ahead of the other BareScript
runtimes. It gets there as a plain bytecode interpreter - no JIT, no assembly, no dependencies -
in 186 KB of code.

The release build is about 1.35x the default build, milliseconds per test run:

| Test                        | C (`-O2`) | C (release) |
| --------------------------- | ---------:| -----------:|
| mandelbrot, 1 run           |        37 |          24 |
| markdownParse, 250 runs     |       899 |         674 |
| qrcodeMatrix, 30 runs       |        76 |          55 |
| schemaValidate, 250 runs    |        72 |          49 |
| urlDecode, 2000 runs        |        27 |          19 |

And parsing is its own story, because the parser is an interpreted BareScript script in every
implementation. Running the include library's full test suite, which parses about 2 MB of
BareScript before it runs a single test:

| Implementation            | Time  |
| ------------------------- | -----:|
| C (release)               | 0.28s |
| C (`-O2`)                 | 0.36s |
| JavaScript                | 1.81s |
| Python (with C extension) |  7.5s |
| Python                    | 10.2s |

Most of that margin is the runtime optimization pass described under [Design](#design) - the
emit-time stack sizing, per-site global caches, threaded dispatch, statement stripping, lazy
treaps, and free lists. Measured on one machine (an Apple M3 Max) before and after that pass, with
the release build:

| Measurement                                   | Before | After  |
| --------------------------------------------- | ------:| ------:|
| Include library test suite (`make test-include-run`) | 0.42s  | 0.32s  |
| ... instructions retired                      | 7.75G  | 5.65G  |
| ... peak memory                               | 87 MB  | 69 MB  |
| mandelbrot, ms per run                        |     39 |     26 |
| markdownElements, ms per 1000 runs            |    501 |    402 |
| markdownParse, ms per 250 runs                |    967 |    524 |
| qrcodeMatrix, ms per 30 runs                  |     84 |     68 |
| schemaParse, ms per 250 runs                  |     62 |     42 |
| schemaValidate, ms per 250 runs               |     71 |     55 |
| urlDecode, ms per 2000 runs                   |     28 |     20 |
| urlEncode, ms per 2000 runs                   |     21 |     15 |

The shared library is 469 KB, of which 202 KB is the compressed include library and 186 KB is
code.

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


## License

Licensed under the MIT License. See [LICENSE](LICENSE).
