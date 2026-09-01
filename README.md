# bare-script-c

A pure-C implementation of the [BareScript](https://craigahobbs.github.io/bare-script/language/)
runtime. The build produces a shared library with the runtime exports and the `bare` command-line
interface.

BareScript is a simple, lightweight, and portable programming language with a Pythonic syntax
influenced by JavaScript, C, and the Unix shell.

```sh
make compile
./build/bare -c 'systemLog("Hello, World!")'
```


## Contents

- [Build](#build)
- [Command-Line Interface](#command-line-interface)
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
- [Performance](#performance)
- [Compatibility](#compatibility)


## Build

The project builds with any C11 compiler and GNU Make, and has no required dependencies beyond
libm. If libcurl is present, HTTP support is compiled in automatically.

```sh
make                # show the available targets
make compile        # build build/libbarescript.{so,dylib}, build/libbarescript.a, and build/bare
make test           # build and run the unit tests
make cover          # run the unit tests and report line coverage; fails under 100%
make test-include   # run the BareScript language test suite with the built CLI
make perf           # run the performance suite
make release        # profile-guided optimization build in build/release
make includes       # regenerate the bundled include library source
make clean          # remove the build directory
make install        # install to $(PREFIX), default /usr/local
```

### Release Builds

`make release` is a three-stage profile-guided build: compile instrumented, run a training
workload, then recompile with the profile and link-time optimization. It is worth about 1.3x over
the default `-O2` build.

```sh
make release
./build/release/bare script.bare
```

The training workload is `perf/train.bare`. A PGO profile is only as good as the workload that
produces it - the optimizer lays out branches and inlines call sites in the proportion the training
run exercises them - so the workload deliberately spans the four phases a real program spends time
in, at roughly the ratio a realistic mix of programs does:

| Phase      | What it covers                                                          |
| ---------- | ----------------------------------------------------------------------- |
| Parsing    | the interpreted parser driving the regex engine and string library       |
| Loading    | JSON decoding and model-to-runtime conversion, the system include path   |
| Evaluating | the statement loop and expression evaluator; numbers, strings, calls     |
| Library    | regex matching, JSON round trips, sorting, datetime formatting           |

The release target also trains on the performance suite, the BareScript language test suite, and a
static analysis run, so the linter and the CLI's own paths are represented.

`make test` and `make cover` accept a `TEST` variable that filters test cases by name substring:

```sh
make test TEST=regex
```


## Command-Line Interface

```
usage: bare [-h] [-c CODE] [-d] [-s] [-x] [-v VAR EXPR] [--version] [file ...]

The BareScript command-line interface

positional arguments:
  file           files to process

options:
  -h, --help     show this help message and exit
  -c, --code     execute the BareScript code
  -d, --debug    enable debug mode
  -s, --static   perform static analysis
  -x, --staticx  perform static analysis with execution
  -v, --var      set a global variable to an expression value
  --version      show the version and exit
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
`json.h`, `regex.h`, `options.h`, the generated `includeSource.h`, and the `barescript.h` umbrella
header.


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
    } u;
} BSValue;
```

The reference counting rules are uniform:

- A function that **returns** a `BSValue` returns an *owned* reference; the caller releases it.
- A function that **takes** a `BSValue` takes a *borrowed* reference; it retains the value only if
  it keeps it beyond the call.
- Container accessors (`bsArrayGet`, `bsObjectGet`) return *borrowed* references.

**Strings** are immutable, reference-counted UTF-8 buffers that cache their code point length, so
an all-ASCII string - the common case - indexes by byte. String library functions index by Unicode
code point.

**Arrays** are vectors of values with amortized growth.

**Objects** are binary search trees of key/value pairs, ordered by key. The tree is a *treap*: a
binary search tree that also satisfies a max-heap property on a pseudo-random per-node priority.
This keeps it balanced in expectation without the bookkeeping of an AVL or red-black tree, which
matters because BareScript code routinely inserts keys in sorted order - the worst case for a
plain binary search tree. Nodes are additionally threaded on a doubly-linked list in insertion
order, so objects iterate in insertion order (matching the reference implementations, whose
objects are JavaScript objects and Python dictionaries) while the tree still provides the sorted
traversal that JSON encoding and value comparison are defined over.

Allocation failure is fatal: there is no useful way for a script runtime to continue without
memory, and threading an out-of-memory result through every value operation would obscure the code
for a case that cannot be tested.


### The Function Format

```c
typedef BSValue (*BSFunctionFn)(const BSValue *args, size_t argCount, BSOptions *options, void *data);
```

- `args` is a **borrowed** array of `argCount` values - never an allocated array object, so a call
  costs nothing beyond the argument evaluation itself. The evaluator uses an inline buffer for up
  to eight arguments.
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
    if (!bsArgsValidate(arrayGetArgs, 2, args, argCount, values, options, "arrayGet")) {
        return bsNull();
    }
    ...
}
```


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

`bsFetchHTTP` is a libcurl implementation compiled in when libcurl is found at build time;
`bsFetchHTTPAvailable` reports whether it is. Without libcurl the file system fetch functions still
work and URL fetches fail.


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
   compiled script -> executed by the runtime, which is what parses your script
```

The model that comes back is converted to the runtime's compiled representation - statements and
expressions as C structs rather than objects - by `model.c`. Two resolution passes run once a
statement list is complete:

- **Jump resolution** turns each jump's label into a statement index.
- **Slot resolution** collects a function's local variables - its declared arguments plus every
  assignment target in its body, a statically known set - and resolves each variable reference to
  a slot index, so a local read is an array load rather than a dictionary lookup. A slot holding
  the internal unset marker falls through to the globals object, matching the reference behavior
  where an unassigned local simply is not a key of the locals dictionary.

`bsScriptToModel`, `bsStatementToModel`, and `bsExprToModel` convert back, which is how the linter
receives a script and how `barescriptEvaluateExpression` works.


### The Bundled Include Library

The thirty scripts of the BareScript include library - `args.bare`, `markdown.bare`, `schema.bare`,
`unittest.bare`, and the rest - are compiled to JSON script models and embedded in the library.
Including one costs a JSON decode rather than a run of the parser.

The models are dictionary compressed with the same scheme the JavaScript implementation uses for
`includeSource.js`: a table of 61 phrases indexed by `[a-zA-Z0-9]`, where encoding replaces each
phrase with `~` plus its index character and a literal `~` escapes to the one index past the last
phrase. The phrase table itself is mined from the corpus - repeatedly taking the substring that
saves the most bytes and removing it - so it tracks the JSON that BareScript's key-sorting
`jsonStringify` actually emits. It compresses 1.58 MB of models to 590 KB.

`src/includeSource.c` and `include/barescript/includeSource.h` are generated and checked in, so a
fresh clone builds with no bootstrap. `make includes` regenerates them by running
`bin/includeSource.bare` - itself a BareScript program - under a CLI built from the *existing*
generated source, the same self-hosting cycle the JavaScript implementation uses.

The generated header exports a stub accessor per include, returning its decoded JSON model:

```c
const char *bsIncludeSourceUnittest(void);      /* unittest.bare */
const char *bsIncludeSourceMarkdownUp(void);    /* markdownUp.bare */
/* ... one per bundled script ... */

extern const BSIncludeSourceFn bsIncludeSourceStubs[BS_INCLUDE_COUNT];   /* all of them, in order */
```

plus `bsIncludeCount`, `bsIncludeName`, and `bsIncludeSource` for lookup by name. A model decodes
on first use and is cached, so a program that includes two of the thirty pays for two.


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
Four properties keep it well-behaved on real input - which matters more here than in the reference
implementations, because the parser is itself regex-driven:

- A pattern whose every alternative begins with `^` only tries the search start position. Every
  pattern the parser uses is anchored this way, so this is the difference between a linear and a
  quadratic scan of each line it parses.

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
make test-include   # the BareScript language test suite, run through the built CLI
```

`make cover VERBOSE=1` lists every uncovered line.

The C unit tests self-register, so adding one is a single `TEST(name) { ... }` block. Coverage is
gathered with `gcov`/`llvm-cov` and summarized by `test/coverage.awk`, which honors
`GCOV_EXCL_LINE` and `GCOV_EXCL_START`/`GCOV_EXCL_STOP` markers - used only for out-of-memory
aborts and platform-specific fallbacks that unit tests cannot reach.

The BareScript-level suite in `test/include` is written against a small self-contained harness so
it runs unchanged on the JavaScript and Python implementations, which makes it a cross-runtime
conformance check as well as a test of this one.

This implementation is additionally validated against the reference BareScript include library's
own test suite - 1372 tests and 16,754 assertions covering the parser, runtime, library, regex
engine, JSON, and coverage instrumentation - which passes with a report byte-identical to the
JavaScript implementation's:

```sh
cd /path/to/bare-script/lib/include/test
BARESCRIPT_INCLUDE_PATH=/path/to/bare-script/lib/include \
    /path/to/bare-script-c/build/bare -c 'include <markdownUp.bare>' \
    -v vUnittestReport true runTests.bare
```


## Performance

`make perf` runs a suite written in plain BareScript - no include library dependencies - so the
same file runs on all three implementations. Elapsed milliseconds, lower is better:

| Test          | C (`-O2`) | C (release) |  JS |
| ------------- | ---------:| -----------:| ---:|
| mandelbrot    |        87 |          59 | 278 |
| arraySort     |        18 |          14 |  27 |
| objectTree    |        28 |          26 |  39 |
| stringBuild   |         9 |           7 |  11 |
| jsonRoundTrip |        16 |          14 |  18 |
| regexMatch    |        12 |          11 |   7 |
| functionCall  |        40 |          31 | 113 |
| **total**     |   **210** |     **162** | **493** |

The release build is 1.3x the default build and 3.0x the JavaScript implementation on this suite.
The regex benchmark is the one place JavaScript wins, against V8's JIT-compiled regular expression
engine.

Parsing is a separate story, because the parser is an interpreted BareScript script in every
implementation. Running the reference include library's full test suite - which parses about
200 KB of BareScript before it runs a single test:

| Implementation | Time  |
| -------------- | -----:|
| C (release)    | 1.91s |
| C (`-O2`)      | 2.12s |
| JavaScript     | 1.47s |
| Python         | 5.51s |

(The Python implementation runs its own C extension for the runtime core, so its numbers are not a
pure-Python baseline.)


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


## License

Licensed under the MIT License. See [LICENSE](LICENSE).
