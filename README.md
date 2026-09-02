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
- [Compatibility](#compatibility)


## Build

The project builds with any C11 compiler and GNU Make, and has no required dependencies beyond
libm. If libcurl is present, HTTP support is compiled in automatically.

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
workload, then recompile with the profile and link-time optimization. It is worth about 1.2x over
the default `-O2` build on the performance suite, and takes about ten seconds. `make install`
installs it.

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
| **PGO, `-O3 -flto`**     | 0.82x | **the release build**                               |

`-Os` is the interesting one, since trading code size for instruction cache residency often wins
in an interpreter. It does not here: `-Os` costs 7% and `-Oz` 43%, and PGO does not close the gap.
The dispatch loop and the value operations are small and hot enough that the inlining `-O2` and
`-O3` do is worth more than the 11% of text section `-Os` gives back.

Link-time optimization is the single largest flag-level win - 7% on its own, and still 6% on top
of PGO - because the value system is small functions across translation unit boundaries:
`bsRetain`, `bsRelease`, and the object treap's comparisons are called from everywhere and can
only be inlined across the library at link time.

The release static library keeps the profile but drops link-time optimization, so it stays an
archive of ordinary object files rather than one of compiler intermediate code, which not every
consumer's linker can read.

The training mix is four programs, merged by execution count. A PGO profile is only as good as the
workload that produces it - the optimizer lays out branches and inlines call sites in the
proportion the training run exercises them.

| Program | Role |
| ------- | ---- |
| `perf/test.bare` | the official suite - most of the counters |
| `perf/train.bare` | source-parse complement: the interpreted parser over include-library `.bare` files. The suite never does this; it loads bundled JSON models. This is the path `bare script.bare` takes. |
| `test/include/runTests.bare` | a small slice of the evaluator on this project's own scripts |
| `bare -s test/include/testLibrary.bare` | the CLI static-analysis path and the linter |

`perf/train.bare` is parse-heavy on purpose. Its edge-count overlap with the suite is only about
20%; doubling the suite in training made the suite slower, so that complementary parse mix is
load-bearing even though it is the shorter run.

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

**Strings** are immutable, reference-counted UTF-8 buffers. The header is 40 bytes plus a pointer
to a NUL-terminated payload allocated with it. They cache their code point length, so an
all-ASCII string - the common case - indexes by byte. Construction skips the UTF-8 walk when the
buffer has no high bit. Non-ASCII indexing keeps a cursor and, after a backward lookup, a sparse
stride-16 offset table. String library functions index by Unicode code point.

**Arrays** are vectors of values with amortized growth.

**Objects** are binary search trees of key/value pairs, ordered by key. The tree is a *treap*: a
binary search tree that also satisfies a max-heap property on a pseudo-random per-node priority.
This keeps it balanced in expectation without the bookkeeping of an AVL or red-black tree, which
matters because BareScript code routinely inserts keys in sorted order - the worst case for a
plain binary search tree. Nodes are additionally threaded on a doubly-linked list in insertion
order, so objects iterate in insertion order (matching the reference implementations, whose
objects are JavaScript objects and Python dictionaries) while the tree still provides the sorted
traversal that JSON encoding and value comparison are defined over. C-string keys and compiled
names of at most 64 bytes are interned, so a hit can compare interned `BSString` pointers
instead of `memcmp`. JSON and computed keys reuse an interned name when it is already in the
table and otherwise stay ordinary strings, so untrusted unique keys cannot grow the table.
The intern table is also capped. Interned names on the compiled script skip hashing entirely.
Objects of more than 32 keys keep an interned-pointer hash table for lookup; a miss there is
definitive unless the object also has uninterned keys. The intern table holds one
reference; interned strings live until process exit.

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

When `__barescriptCoverage` is enabled, each compiled script keeps a line-indexed array of
pointers into the coverage object's per-line counts, so a loop increments a number instead of
formatting a line key and searching the covered object on every statement.

`bsScriptToModel`, `bsStatementToModel`, and `bsExprToModel` convert back, which is how the linter
receives a script and how `barescriptEvaluateExpression` works.


### The Bundled Include Library

The thirty-two scripts of the BareScript include library - `args.bare`, `markdown.bare`,
`schema.bare`, `unittest.bare`, `gzip.bare`, `base64.bare`, and the rest - are compiled to JSON
script models and embedded in the library. Including one costs a JSON decode rather than a run of
the parser.

The models are gzip-compressed at level 9 by `gzip.bare` (`gzipCompress` / `gzipUncompress`, byte
arrays in and out) and base64-encoded by `base64.bare` (`base64Encode` / `base64Decode`) for
embedding as C string literals. That compresses about 598 KB of include library source to 267 KB
of embedded text (45%). `bin/includeSource.bare` serializes the object and array structure itself,
in `objectKeys` (insertion) order, and delegates only leaf values to `jsonStringify` so number
formatting and string escaping stay exactly what the runtime produces.

`src/includeSource.c` and `include/barescript/includeSource.h` are generated and checked in, so a
fresh clone builds with no bootstrap. `make includes` regenerates them by running
`bin/includeSource.bare` - itself a BareScript program - under a CLI built from the *existing*
generated source, with `BARESCRIPT_INCLUDE_PATH` pointing at `lib/include` so `gzip.bare` and
`base64.bare` are available before they are bundled.

The generated header exports a stub accessor per include, returning its decoded JSON model:

```c
const char *bsIncludeSourceUnittest(void);      /* unittest.bare */
const char *bsIncludeSourceMarkdownUp(void);    /* markdownUp.bare */
/* ... one per bundled script ... */

extern const BSIncludeSourceFn bsIncludeSourceStubs[BS_INCLUDE_COUNT];   /* all of them, in order */
```

plus `bsIncludeCount`, `bsIncludeName`, and `bsIncludeSource` for lookup by name. A model decodes
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
ASCII subjects match the original bytes without widening to a `uint32_t` buffer. Four properties
keep it well-behaved on real input - which matters more here than in the reference implementations,
because the parser is itself regex-driven:

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
make test-include   # the BareScript include library test suite
make test-language  # this project's own BareScript language tests
```

`make cover VERBOSE=1` lists every uncovered line.

The C unit tests self-register, so adding one is a single `TEST(name) { ... }` block. Coverage is
gathered with `gcov`/`llvm-cov` and summarized by `test/coverage.awk`, which honors
`GCOV_EXCL_LINE` and `GCOV_EXCL_START`/`GCOV_EXCL_STOP` markers - used only for out-of-memory
aborts, platform-specific fallbacks, and two checks that guard against a corrupted parser.

### The Include Library Test Suite

`make test-include` runs the BareScript include library's own test suite - vendored under
`lib/include/test` - as the JavaScript and Python implementations run it, in the same three parts:

| Target                    | What it runs                                                    |
| ------------------------- | --------------------------------------------------------------- |
| `test-include-run`        | 1372 tests, 16,754 assertions, at 100% BareScript-level coverage |
| `test-include-markdownup` | the 22 `markdownUp.bare` tests                                   |
| `test-include-lint`       | static analysis of all 65 library and test scripts               |

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
this runtime by about 1.2x against them.

```sh
make perf
make perf TEST=mandelbrot PERF_RUNS=5
```

Milliseconds per 1000 runs, best of two, on one machine - lower is better:

| Test             | BareScript (C) | BareScript (JS) | BareScript (PyC) | BareScript (Py) |
| ---------------- | --------------:| ---------------:| ----------------:| ---------------:|
| mandelbrot       |     **80,000** |         308,000 |          110,000 |       3,528,000 |
| markdownElements |          1,254 |             746 |          **661** |           5,252 |
| markdownParse    |          7,724 |       **3,120** |            7,380 |          21,080 |
| qrcodeMatrix     |      **6,033** |          13,167 |            9,067 |         124,667 |
| schemaParse      |        **732** |           1,216 |            1,252 |           9,156 |
| schemaValidate   |        **828** |           1,968 |            1,060 |          14,412 |
| urlDecode        |       **42.5** |              91 |              132 |             690 |
| urlEncode        |         **30** |            51.5 |               57 |             394 |

The C runtime is the fastest BareScript runtime on six of the eight tests. (`BareScript (PyC)` is
the Python implementation running its C extension for the runtime core, so it is not a pure-Python
baseline; `BareScript (Py)` is.) The two it loses are the markdown tests: `markdownParse` goes to
JavaScript, against V8's JIT-compiled regular expression engine - it is almost entirely regex work
- and `markdownElements` to the Python C extension.

Two other numbers are worth having. The release build is 1.3x the default build:

| Test          | C (`-O2`) | C (release) |
| ------------- | ---------:| -----------:|
| mandelbrot    |        87 |          59 |
| functionCall  |        40 |          31 |
| arraySort     |        18 |          14 |
| stringBuild   |         9 |           7 |

And parsing is its own story, because the parser is an interpreted BareScript script in every
implementation. Running the include library's full test suite, which parses about 200 KB of
BareScript before it runs a single test:

| Implementation | Time  |
| -------------- | -----:|
| JavaScript     | 1.47s |
| C (release)    | 1.91s |
| C (`-O2`)      | 2.12s |
| Python         | 5.51s |

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
