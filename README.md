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
470 KB library with no dependency beyond libm that starts in 3 ms and 3 MB. The measurements are
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
variable names the includes to bundle, `INCLUDE_EXCLUDE` the ones to leave out, and either defines
the macro for every include not bundled:

```sh
make release INCLUDE="barescriptParser.bare barescriptLint.bare markdownUp.bare url.bare"
make release INCLUDE_EXCLUDE="qrcode.bare draw.bare"
```

A compiled-out include keeps its registry entry and stub accessor, which return no model, so
`include <name.bare>` is served from the system include path when one is registered and fails
otherwise. There is no dependency tracking: an include that an included script itself includes has
to be listed with it - `markdownUp.bare` includes four scripts that include five more - and an
excluded include is missing from every bundled include that includes it. The parser and linter
alone make a 286 KB release library, against 470 KB with all thirty-two. A change to `INCLUDE` or
`INCLUDE_EXCLUDE` needs a `make clean` first, and the test suites need every include.


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

On real-world code this runtime beats V8's bytecode interpreter, CPython, Lua, Ruby, and Perl,
starts in 3 ms and 2.6 MB, and does it as a plain bytecode interpreter - no JIT - in 227 KB of
code. Two suites back that up: `make perfx`, real-world-like applications ported to six
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
| JavaScript (V8 JIT)     | **16.9 ms** |     494 ms | **107 ms** | **129 ms** |  **140 ms** |   1.00x |
| BareScript              |      442 ms | **435 ms** |     119 ms |     526 ms |      242 ms |   2.83x |
| JavaScript (V8 jitless) |      784 ms |     946 ms |     166 ms |     731 ms |      338 ms |   4.53x |
| Python                  |      894 ms |     1.00 s |     299 ms |     771 ms |      405 ms |   5.54x |
| Lua                     |      586 ms |     948 ms |     1.24 s |     459 ms |      619 ms |   6.56x |
| Ruby                    |      1.13 s |     1.01 s |     494 ms |     1.06 s |      1.03 s |   8.25x |
| Perl                    |      2.06 s |     1.14 s |     4.44 s |     2.82 s |      991 ms |  17.86x |

Only V8's JIT is faster overall, and BareScript beats it on the regex test. It leads every
interpreter on the regex, JSON, and n-body tests, and trails Lua 1.1x on pathfinding, where every
element access is a library call. (Lua's JSON is a pure-Lua codec; Perl's is its core
`JSON::PP`.) Launching the empty program:

| Language            |    Wall | Peak RSS |
| ------------------- | ------: | -------: |
| Lua                 |  2.5 ms |   1.7 MB |
| BareScript          |  2.8 ms |   2.6 MB |
| Perl                |  4.4 ms |   4.3 MB |
| Python              | 16.2 ms |  14.6 MB |
| JavaScript (V8 JIT) | 25.9 ms |  38.3 MB |
| Ruby                | 43.3 ms |  28.0 MB |

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
| JavaScript (V8)  | **1.56 s** | **32.2 ms** | **617 ms** | **56.5 ms** | **2.2 ms** |           |   1.00x |
| BareScript (C)   |     12.0 s |      203 ms |     788 ms |      128 ms |     4.0 ms | **160 s** |   2.26x |
| Python (CPython) |     45.8 s |             |            |      205 ms |    10.4 ms |           |   4.21x |
| BareScript (JS)  |      258 s |      665 ms |     2.17 s |      1.77 s |    42.5 ms |   1,590 s |  20.03x |
| BareScript (PyC) |      108 s |      622 ms |     7.31 s |      1.06 s |    52.0 ms |   7,170 s |  25.25x |
| BareScript (Py)  |    3,511 s |      5.24 s |     21.2 s |      14.0 s |     370 ms |   9,460 s | 176.48x |

The closest race is `markdownParse`, regular expressions against V8's JIT-compiled regex engine;
the widest is `mandelbrot`, arithmetic against the JIT, then `markdownElements`, V8's inline caches
allocating nested objects 6.3x faster. Native C runs `mandelbrot` in 0.79 ms, 15x ahead.

### Memory and Size

Peak resident set of each perfx application, `empty` being the empty program, with the smallest
per column in bold:

| Language                |      empty |      nbody |   loganalyze |      jsonetl |    pathfind | salesreport | vs best |
| ----------------------- | ---------: | ---------: | -----------: | -----------: | ----------: | ----------: | ------: |
| Lua                     | **1.7 MB** | **1.8 MB** |     140.1 MB |     201.5 MB |     39.4 MB | **37.2 MB** |   1.00x |
| BareScript              |     2.6 MB |     2.9 MB |  **97.4 MB** | **120.2 MB** | **32.1 MB** |     69.5 MB |   1.08x |
| Perl                    |     4.3 MB |     6.6 MB |     136.5 MB |     219.9 MB |    113.2 MB |     86.6 MB |   2.02x |
| Python                  |    14.6 MB |    15.2 MB |     106.9 MB |     161.8 MB |     56.4 MB |     73.5 MB |   2.25x |
| Ruby                    |    28.0 MB |    28.1 MB |     189.0 MB |     206.3 MB |     43.1 MB |    105.3 MB |   3.23x |
| JavaScript (V8 JIT)     |    38.3 MB |    44.8 MB |     363.3 MB |     159.5 MB |     69.8 MB |    194.3 MB |   4.71x |
| JavaScript (V8 jitless) |    37.7 MB |    40.8 MB |     357.0 MB |     181.4 MB |     86.0 MB |    196.1 MB |   4.89x |

BareScript is single-threaded, so its CPU time equals its wall time; V8 spends up to a third more
CPU than wall on background threads. Lua interns every short string, which halves its sales
report footprint, where the CSV fields repeat.

| This runtime                            |        |
| --------------------------------------- | -----: |
| Shared library                          | 470 KB |
| ... of which compressed include library | 159 KB |
| ... of which Unicode case tables        |   8 KB |
| ... of which code                       | 227 KB |
| Empty script, resident set              | 2.5 MB |
| Empty script, peak footprint            | 1.8 MB |
| `make perf` test, peak                  |   5 MB |
| Include library test suite, peak        |  33 MB |

The empty script's floor is the process itself: libcurl loads on the first HTTP fetch, the parser
compiles from its model a statement at a time without building the model's objects, and a
script keeps its model only where lint or
coverage reads it. Memory figures are from `/usr/bin/time -l`.

## Compatibility

This runtime defines BareScript's behavior. Its regular expressions and its Unicode whitespace
and case behavior are standard JavaScript's; the other ports - the JavaScript and Python
implementations - are the same as this one, within reason. The three match, including error
messages and their column numbers, except as recorded here: where the two ports disagree with each
other, this implementation's behavior is the one shown in bold.

| Behavior                               | JavaScript        | Python          | This implementation |
| -------------------------------------- | ----------------- | --------------- | ------------------- |
| String length and indexing             | UTF-16 code units | code points     | **code points**     |
| `stringDecode` of a non-number element | a NUL character   | `null`          | **`null`**          |
| An unmatched capture group in a match  | omitted           | `null`          | **omitted**         |
| Zero-width `regexSplit` matches        | never split at the last split's end | split at each | **never split at the last split's end** |
| `$&`, `` $` ``, `$'` in a `regexReplace` replacement | the match, the text before it, the text after it | literal text | **the match, the text before it, the text after it** |
| A repeated group's captures, `(a*)*` on `a` | unset each iteration, an empty iteration past the minimum rejected | the last iteration's | **as JavaScript** |
| A lookbehind body                      | matched right to left, so `(?<=(\w+) )x` captures the word | left to right at each length | **right to left** |
| A group name reused within one alternative, `(?<n>a)(?<n>b)` | `redefinition`  | `redefinition`  | **`redefinition`** |
| A group name reused across an alternation's branches, `(?<n>a)\|(?<n>b)` | the branch that matched | `redefinition` | **the branch that matched** |
| A named backreference before its group, `\k<n>(?<n>a)` | matches empty, then the group | `unknown group name` | **matches empty, then the group** |
| A group name beginning with a digit, or with a non-ASCII letter | a digit rejected, a letter accepted | the same | **both rejected - a name is a BareScript identifier** |
| A quantified lookahead, `(?=a)*`       | accepted          | accepted        | **`nothing to repeat`** |
| A repeated flag, `regexNew('a', 'ii')` | `null`            | accepted        | **`null`**          |
| `String(1e-7)`                         | `1e-7`            | `1e-07`         | **`1e-7`**          |
| `String(-0)`                           | `0`               | `-0`            | **`0`**             |
| `-7 % 3`                               | `-1` (truncated)  | `2` (floored)   | **`-1`**            |
| Bitwise operators                      | 32-bit            | arbitrary width | **32-bit**          |
| A `regexNew` repeat count past 2^32    | accepted          | uncaught error  | **accepted**, saturating at 2^31 - 1 |
| `numberToString` past 2^53             | shortest round trip, exponential past 1e21 | the value's exact digits | **the value's exact digits** |
| A `systemFetch` array                  | fetched concurrently | fetched in order | **URLs concurrently, then files in order** |
| `numberToFixed` from 1e21              | exponential, `1e+21` | the value's digits; `null` when scaled past the double range | **the value's digits; `null` when scaled past the double range** |
| A datetime past year 9999              | JavaScript's `Date` range, 8.64e15 ms either side of the epoch | `null`, an error | **JavaScript's `Date` range** |
| `numberToFixed` past 100 digits        | `null`, a `RangeError` | the digits    | **`null`**          |
| `stringFromCharCode` past 0xFFFF       | the low 16 bits   | the code point  | **the code point**  |
| `regexEscape`                          | the metacharacters | also `-`, `#`, `&`, `~`, and whitespace | **the metacharacters** |
| A relative path normalizing to nothing, `a/..` | the empty string | `.`      | **`.`**             |
| Unicode whitespace past the spaces both match | also U+FEFF   | also U+0085 and U+001C-U+001F | **also U+FEFF** |
| `\w`, `\d`, `\b`                       | ASCII             | Unicode letters and digits | **ASCII**     |
| `.` and multi-line `^` `$` at CR, U+2028, U+2029 | line terminators | LF only    | **line terminators** |
| `$` before a trailing LF               | no match          | matches         | **no match**        |
| The `i` flag on ß, ı, İ, K (Kelvin), ſ | not folded        | folded to ẞ, I, i, k, s | **not folded** |

`objectKeys` returns keys in insertion order, matching both references for ordinary keys.
JavaScript additionally hoists integer-like keys to the front in ascending numeric order; this
implementation does not, matching Python.

BareScript's regular expressions are standard JavaScript's - their syntax, and the matching
semantics of every construct: the rows above record where the Python port differs. Where
JavaScript accepts a legacy form that can only be a mistake, correctness wins over its behavior:
a backreference to a group the pattern never defines (`(a)\2`, an octal escape in JavaScript), an
incomplete `\x4` or `\u12`, `\8` and `\9`, and `\k<n>` in a pattern with no named groups are
errors here - as they are in JavaScript's unicode mode - and a quantified lookaround is an error
whichever way it looks. The `regexNew` messages section below lists these with Python's messages.

BareScript's Unicode whitespace and case behavior is standard JavaScript's. Whitespace - the
regex `\s` class, `stringTrim`, and the space around a parsed number - is
JavaScript's WhiteSpace and LineTerminator sets: the ASCII spaces, U+00A0, U+1680, U+2000-U+200A,
U+2028, U+2029, U+202F, U+205F, U+3000, and U+FEFF. The regex `\w`, `\d`, and `\b` are ASCII; `.`
and the multi-line anchors know every line terminator - LF, CR, U+2028, U+2029 - and `$` alone is
the end of the string. `stringUpper` and `stringLower` apply Unicode's full case mapping (Unicode
16.0): ß upper-cases to `SS`, the ligatures expand, and a capital sigma that ends a word
lower-cases to the final sigma. The `i` flag folds as JavaScript does: a code point matches the
ones sharing its simple upper case, but never across the ASCII boundary, and never through an
expanding upper case. JavaScript has all of this natively. The Python implementation keeps
Python's own definitions where they differ - the rows above - which is within reason: in
practice the differences are a leading byte-order mark, whitespace here and in JavaScript but not
in Python, and `$` before a trailing newline, which Python's `$` matches.

One capability of the reference implementations is out of scope here:

- **Asynchronous functions.** Like the Python implementation, every function executes
  synchronously; the `async` keyword parses and is recorded in the model, but imposes no
  restriction. Scripts written for the JavaScript runtime run unchanged, and the linter's async
  checks - which need to know which functions are async - are skipped, as they are in Python.
- **A match's group key order.** A match model's `groups` object keys each named group right after
  its number; both references list every number first, then the names. Only `objectKeys` can tell.

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
| `(?<n>a)`, `(?P<n>a)` | a named group, `unknown extension ?P` | `unknown extension ?<n`, a named group |
| `(?>a)`, `(?(1)a)` | `unknown extension`             | atomic groups and conditionals  |
| `a*+`           | `multiple repeat`                  | a possessive quantifier         |
| `x{,5}`         | the literal text                   | the quantifier `{0,5}`          |
| `[]`            | a set that never matches           | `unterminated character set`    |
| `[\k]`, `\cA`     | identity and control escapes       | `bad escape`                    |
| `\1(a)`         | a forward reference, matches empty | `invalid group reference`       |
| `(?<=a*)b`      | a variable-width lookbehind        | `look-behind requires fixed-width pattern` |

Some cases go the other way, each a pattern JavaScript accepts that can only be a mistake. A
numbered backreference to a group the pattern never defines - `(a)\2` - is a legacy octal escape in
JavaScript, matching the control character U+0002; this implementation reports `re`'s `invalid group
reference`. (A reference to a group defined later in the pattern is still a forward reference, as the
table says, and so is a named one - `\k<n>(?<n>a)` - which `re` rejects.) A character class range
with a class escape as either bound - `[\d-z]` - is a literal `-` in JavaScript and `bad character
range` here; a named backreference to a name the pattern never defines - `\k<n>` - is the literal
text in JavaScript and `unknown group name` here; an incomplete hexadecimal escape - `\x4`, `\u12` -
is the literal text in JavaScript and `incomplete escape` here; `\8` and `\9` are the digits in
JavaScript and `invalid group reference` here; a three-digit octal escape past `\377` - `\477`,
which JavaScript reads as `\47` then `7` - is `octal escape value \477 outside of range` here; and a
quantified lookahead - `(?=a)*` - is `nothing to repeat` here, as a quantified lookbehind is in
JavaScript, where both `re` and the unicode-mode JavaScript syntax also reject the lookahead. A group
name shared by two groups of one alternative - `(?<n>a)(?<n>b)` - is `re`'s `redefinition of group
name`, as in JavaScript. This implementation also limits a pattern to 127 capture groups,
reporting `sorry, but this version only supports 127 groups`; both references allow more.

Where a pattern is invalid in both, the message and position match, except for a pattern that is
invalid for two reasons, where each engine reports the one it meets first.


## Design

[DESIGN.md](DESIGN.md) describes how the runtime works - the value system, the register bytecode
and the parser that is itself a BareScript script, the bundled include library, the regular
expression engine, thread confinement - and how the release build and its training workload were
chosen.


## License

Licensed under the MIT License. See [LICENSE](LICENSE).
