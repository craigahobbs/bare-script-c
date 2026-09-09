# bare-script-c Design

How the runtime works and how it was tuned - the reference for changing it. The
[README](README.md) covers building, the command line, embedding, and the measurements.

## Contents

- [The Value System](#the-value-system)
- [The Function Format](#the-function-format)
- [The Fetch Function Format](#the-fetch-function-format)
- [The Parser and Linter](#the-parser-and-linter)
- [The Bundled Include Library](#the-bundled-include-library)
- [JSON](#json)
- [Regular Expressions](#regular-expressions)
- [Threads](#threads)
- [The Release Build](#the-release-build)
- [Testing and Coverage](#testing-and-coverage)


## The Value System

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
buffer has no high bit. Non-ASCII indexing keeps, in an index block allocated on first use, a
cursor and, for a string of thirty-two code points or more, a sparse stride-16 offset table.
String library functions index by Unicode code point. `stringUpper` and `stringLower` apply
Unicode's full case mapping (`unicode.c`): the simple mappings are runs of code points sharing one
delta - stepping by two through the alternating upper/lower blocks - found by binary search, in
16-bit form for the Basic Multilingual Plane and 32-bit form past it, the
few expanding mappings (ß to `SS`, the ligatures) a sorted special table, and a capital sigma that
ends a word - a cased code point before it and none after, case-ignorable code points aside -
lowers to the final sigma. An ASCII string maps byte for byte. The header also caches the
string's content hash, for object lookups, and records the allocation's capacity: the `+`
operator appends in place when its left operand is a function local holding the string's only
reference - so `s = s + piece` in a loop is linear rather than quadratic - growing the allocation
geometrically
(a global loads into a temporary first, and never qualifies). Strings are the runtime's most
frequent allocation and most are short, so an allocation that fits one of five size classes is
rounded up and recycled through that class's free list.

**Arrays** are vectors of values with amortized growth.

**Objects** are an insertion-ordered array of key/value entries, so objects iterate in insertion
order (matching the reference implementations, whose objects are JavaScript objects and Python
dictionaries). Up to three entries are stored in the 104-byte object itself; past that they move
to a heap buffer that doubles as it fills, and past sixteen keys the object builds a hash index
over them - an open-addressing table of `(hash, entry)` slots keyed by the content hash the key
string caches in its header - so a lookup probes the index and compares one key, while a smaller
object scans its entries. The operations defined over sorted keys - JSON encoding, comparison,
`bsObjectKeysSorted` - sort an index of the entries on demand: an insertion sort of a short run,
a merge sort above that.

A lookup compares a stored key to the key sought by pointer first: C-string keys and compiled
names of at most 64 bytes are interned, so a name from compiled code or the library hits without
a `memcmp`, and two distinct interned strings are known to differ without one. An `objectGet` or
`objectSet` call site remembers the entry index its key was last found at, and a record built the
same way as the last one is found at that one compare before any scan. Only when the two
are not both interned are the bytes compared - after the hash and size agree, in an indexed
object. JSON keys reuse an interned name when it is already in the table and otherwise stay
ordinary strings, so untrusted unique keys cannot grow the table, which is also capped. The
intern table holds one reference; interned strings live until the thread's `bsValueCleanup`.
The hash is FNV-1a over the bytes followed by an avalanche step: a multiply alone leaves the low
bits, which the index probes by, depending on one byte of each word, and keys like IP addresses
and paths then cluster.

Allocation failure is fatal: there is no useful way for a script runtime to continue without
memory, and threading an out-of-memory result through every value operation would obscure the code
for a case that cannot be tested.


## The Function Format

```c
typedef BSValue (*BSFunctionFn)(const BSValue *args, size_t argCount, BSOptions *options, void *data);
```

- `args` is a **borrowed** array of `argCount` values - the interpreter's reads of the call's
  operand registers, never an allocated array object, so a call costs nothing beyond the argument
  evaluation itself.
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
`objectGet`, `mathAbs`, and their kin - also carry an *intrinsic* id. The interpreter handles
their happy-path argument shapes itself, without validation, and any other shape falls through
to the function: the nine with one shape - `arrayGet`, `arrayLength`, `arrayPush`, `arraySet`, `objectGet`,
`objectHas`, `objectSet`, `stringLength`, `stringSlice` - as call opcodes of their own, which the emitter chooses by a call's
name and argument count and whose handler first checks that the site's cache is warm for the
activation's globals object and holds that library function (the one function value carrying the
id) - a cold site, or one an expression's locals object could shadow, takes the general call,
which resolves the cache; the rest in the call path's intrinsic switch. An opcode takes about fifteen instructions
off a global `arrayGet` call of ninety-five, which is a tenth of an object-heavy program such as
the `nbody` port.


## The Fetch Function Format

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


## The Parser and Linter

BareScript is parsed by **barescriptParser.bare** and linted by **barescriptLint.bare** - the same
include library scripts the JavaScript and Python implementations use, running on this runtime.
The syntax accepted, the lowering of structured statements, and the exact text and column of every
error message are therefore shared with the reference implementations rather than reimplemented.

That is a bootstrap problem: the parser is a BareScript script, so parsing it would need a parser.
It is solved the way the reference implementations solve it - the bundled parser is stored as its
own parser-compiled model, in a binary encoding that loads with no parser at all.

```
barescriptParser.bare (bundled binary model)    your script's model (the parser's objects)
        |  bsScriptFromModelBinary                       |  bsAstStatement
        v                                                v
             the syntax tree (BSAst) - one statement at a time
                                |  the emitter
                                v
   bytecode chunk  -> executed by the runtime, which is what parses your script
```

`model.c` compiles a transient syntax tree - an arena of nodes holding one statement, reset for the
next - to bytecode. A parsed script's model objects are loaded into the tree a statement at a time;
a bundled include's binary model is read straight into it, so no model object is built for it. The
loaders reject a malformed model, and the emitter
assumes a well-formed tree. Jump labels become instruction indexes and function-local names become
slot indexes during emit - there is no executable expression tree. A slot holding the internal unset marker
falls through to the globals object, matching the reference behavior where an unassigned local
simply is not a key of the locals dictionary. Group nodes stay in the model (the parser and linter
observe them) and flatten only in the code stream.

The code is register code: an eight-byte instruction names a destination register and two
operand registers. A chunk's registers are its slots, then the temporaries the emitter allocates
stack-fashion as it compiles an expression - a subexpression's result lands in the lowest free
temporary, freed again once consumed - then the chunk's constants, copied in when the frame is
built so a literal is read like any other register, with no tag test on the operand; so `x = a +
1` is one instruction that reads the local and the literal in place, and a call's arguments are
operands in the words that follow it, read into a borrowed argument array without a push or a
reference count. The emitter counts each chunk's temporaries, so the caller fills a frame's
registers once - the interpreter never allocates its own - and it compiles a jump's condition as
jumps: a comparison is one comparison jump, `and` and `or` short-circuit through jumps of their
own, and a `not` flips the sense, so `jumpif (a < b && c < d)` is two instructions. The names a call site, load site, or unknown-label trap refers to are not operands
and live in a table of their own, so a frame copies only literals. A local read before
it is assigned falls through to the global of the same name, which would cost every register read
a test; instead the emitter runs a definite-assignment analysis over each function body - a
forward must-analysis across the basic blocks its labels and jumps delimit - and reads a slot
that is definitely assigned as a bare register, leaving the test to the reads that might find the
slot unassigned. Every global
function call, global variable read, and global variable write compiles to a per-site cache that
points at the globals object's value slot for the name; the cache is re-resolved only when a key is added to or removed
from the globals object (its *structural generation*), so an assignment to a global never
invalidates other sites. Under GNU C the interpreter dispatches through a label table, one indirect
branch per opcode. A runtime error is detected at entry, after each call, and at the statements
that raise one; the statement's line number is found from a per-chunk table only when an error
message needs it. Bundled include scripts - the parser, the linter, and the library - are never
statement-counted or coverage-recorded, so the emitter writes no statement markers into their
code.

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


## The Bundled Include Library

The thirty-two scripts of the BareScript include library - `args.bare`, `markdown.bare`,
`schema.bare`, `unittest.bare`, `gzip.bare`, and the rest - are compiled to script models,
encoded in a binary form, and embedded in the library. Including one costs a read of the bytes
rather than a run of the parser: `bsScriptFromModelBinary` reads the model a statement at a time
into the emitter's syntax tree and compiles it before the next, building no model objects, so a
model - about seven times the size of its encoding as objects - is never in memory at all, and
the tree holds one statement.

The encoding, written by `bin/includeSource.bare` and read by `bsScriptFromModelBinary` in
`model.c`: a version byte; a string table - every name, literal, operator, and label once, so the
reader interns each once and a use costs one reference; then the statements. Counts, lengths,
indexes, and line numbers are LEB128 varints, a string a table index, a statement a kind byte and
its members, an expression a tag byte and its members, with an integer a zigzag varint and any
other number its shortest text. The models are gzip-compressed at level 9 by `gzip.bare`
(`gzipCompress` / `gzipUncompress`, byte arrays in and out) and embedded as `unsigned char` arrays.
That compresses about 601 KB of include library source to about 159 KB of gzip - against 204 KB
for the same models as JSON.

`src/includeSource.c` and `include/barescript/includeSource.h` are generated and checked in, so a
fresh clone builds with no bootstrap. `make includes` regenerates them by running
`bin/includeSource.bare` - itself a BareScript program - under a CLI built from the *existing*
generated source, with `BARESCRIPT_INCLUDE_PATH` pointing at `lib/include` so `gzip.bare` is
available before it is bundled.

The generated header exports a stub accessor per include, returning its inflated binary model and
its size:

```c
const unsigned char *bsIncludeSourceUnittest(size_t *size);      /* unittest.bare */
const unsigned char *bsIncludeSourceMarkdownUp(size_t *size);    /* markdownUp.bare */
/* ... one per bundled script ... */

extern const BSIncludeSourceFn bsIncludeSourceStubs[BS_INCLUDE_COUNT];   /* all of them, in order */
```

`runtime.h` adds `bsIncludeCount`, `bsIncludeName`, and `bsIncludeSource` for lookup by name. A model
compiles on first use and is cached, so a program that includes two of the thirty-two pays for two.

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


## JSON

`bsJSONEncode` writes values with object keys in sorted order, which the value system provides for
free. Datetime and function values encode as their `bsValueString` representation and regex values
as null, matching the reference implementations. `bsJSONDecode` parses into a `BSValue`; an
unpaired surrogate in a `\uXXXX` escape becomes U+FFFD so every decoded string is valid UTF-8.


## Regular Expressions

The engine parses a pattern to a node tree, computes first sets from it, compiles it to a linear
program, and frees the tree. One loop runs the program with an explicit backtrack stack: an
alternation's remaining alternatives, a repeat's remaining iterations, and a simple repeat's
give-back are backtrack entries, and every capture and repeat-counter write is on an undo trail
that a backtrack unwinds to the entry's mark - a failed run unwinds to its own mark, so no
capture leaks into the next attempt. Matching costs no C recursion beyond one call per lookaround
body, and a program is about a quarter the size of the tree it replaces. A lookbehind's body is
emitted last node first, with backward forms of the instructions that consume the subject, so it
matches right to left as JavaScript's does: a greedy repeat takes from the right, and a
backreference can name a group to its right. A repeated group's captures start each iteration
unset, and an iteration past the required ones that matches nothing fails, as JavaScript's rules
say; a named backreference resolves after the parse, so it may precede its group, and one to a
name shared across an alternation's branches means the group that took part.
BareScript's regular expressions are standard JavaScript's: the engine implements the subset of
JavaScript's syntax that the regex functions expose, with JavaScript's matching semantics:

| Category   | Syntax                                                                    |
| ---------- | ------------------------------------------------------------------------- |
| Literals   | `.` `[...]` `[^...]` `( )` `(?: )` `(?<name> )` alternation               |
| Lookaround | `(?= )` `(?! )` `(?<= )` `(?<! )`                                          |
| Escapes    | `\d \D \w \W \s \S \b \B \n \r \t \f \v \0 \xHH \uHHHH \k<name>` `\1`-`\9` |
| Repeats    | `* + ? {n} {n,} {n,m}` and their lazy `?` forms                             |
| Anchors    | `^ $`                                                                      |
| Flags      | `i` (case-insensitive), `m` (multi-line), `s` (dot matches newline)         |

`\s` is the Unicode whitespace set README's Compatibility section defines (`bsIsSpaceCode`, shared
with `stringTrim` and number parsing); `\w`, `\d`, and `\b` are ASCII; `.` and the multi-line
anchors stop at every line terminator (LF, CR, U+2028, U+2029). The `i` flag folds by
JavaScript's rule: a code point's canonical form is its simple upper case unless that crosses
from non-ASCII to ASCII (`bsUnicodeCanon`, with the ASCII letters folded in line), a literal is
stored canonical and compared canonical, and a class matches a code point when it holds any
member of the code point's canonical group - the canonical form, its lower case when that maps
back, and the further members of the twenty-odd groups with several lower-case forms
(`bsUnicodeCanonMembers`). The ASCII membership table folds the same way at compile time.

Matching is over Unicode code points, so match indexes agree with the string library's indexes.
ASCII subjects match the original bytes without widening to a `uint32_t` buffer. Five properties
keep it well-behaved on real input - which matters more here than in the reference implementations,
because the parser is itself regex-driven:

- A pattern whose every alternative begins with `^` only tries the search start position. Every
  pattern the parser uses is anchored this way, so this is the difference between a linear and a
  quadratic scan of each line it parses.
- An unanchored pattern computes the set of code points a match can begin with - a character
  class contributes its finished membership, predefined classes included - and the search skips
  every position whose code point is not in it: over an ASCII subject with `memchr` for a set of
  one byte and a byte table otherwise. An alternation indexes its alternatives by first code
  point - a byte of alternative bits per ASCII code point for up to eight alternatives, a word
  for up to sixty-four, one mask for the code points past ASCII - so it tries only the
  alternatives that can begin at a position: the markdown span alternation has sixteen, a
  highlight keyword list up to sixty-two, and a position usually admits one or two. A wider
  alternation keeps a first set per alternative and tries them one at a time.
- A quantifier whose body matches exactly one code point - `\s*`, `[0-9]+`, `.*`, the overwhelming
  majority of real patterns - scans its run in one loop and gives back one position at a time
  through a single backtrack entry, skipping the positions that cannot hold a literal that follows.
- Capture writes are recorded on an **undo trail**, so backtracking out of a lookaround restores
  state in time proportional to what changed rather than copying the capture array.
- Backtracking steps are **budgeted**, so a pathological pattern gives up rather than hanging the
  runtime, and the backtrack stack lives on the heap, so no pattern can overflow the C stack.


## Threads

The runtime has no process-wide mutable state. Every free list, the intern table, the interned
model keys, the library's function values, the compiled parser, linter, and bundled include caches,
the system include registry and search path, and the random number generator state are
`_Thread_local`, so each thread is an independent runtime and threads never contend - there are no
locks. The one exception is libcurl, which the first thread to fetch a URL loads and globally
initializes behind a C11 atomic, because `curl_global_init` was not thread-safe before libcurl 7.84.
Each thread then fetches through a libcurl multi handle of its own, which holds its connection pool
that `bsLibraryCleanup` releases, with libcurl's signal handling off so that no transfer touches the
process's SIGPIPE disposition.

What this buys is confinement, and confinement is the rule: values, scripts, expressions, and
options belong to the thread that created them and cannot be handed to another. Reference counts
are plain integers, and an interned string is known only to its own thread's table, so a key that
crossed threads would be compared by pointer against strings it can never equal. Pass text between
threads instead - a script's source, or a JSON string - and let the receiving thread parse it.

Each thread pays for its own copy of what it uses. The parser and linter bootstrap once per thread
that parses, an include compiles once per thread that includes it, and the free lists and intern
table fill per thread - about what the command-line interface's startup costs, a few milliseconds
and under 2 MB. A thread that used the runtime releases that state before it exits with
`bsParserCleanup`, `bsSystemIncludeClear`, `bsIncludeCleanup`, `bsLibraryCleanup`, and last
`bsValueCleanup`, which frees the free lists and the intern table; a thread that exits without them
leaks its copy. The command-line interface is single-threaded and calls the five at exit. The C
unit tests run eight threads through the parser, linter, includes, and library at once, and the
suite passes under ThreadSanitizer.

Thread-local storage is not free: on Mach-O every access to a thread-local variable is a call to
the loader's thunk, so each file keeps its state in one struct and a function computes the address
once, and the interpreter touches none of it on a function call. What remains is one address
computation per free-list push or pop and per interned-key lookup - on the performance suite, 1-3%
more instructions and 1-4% more cycles than the process-global runtime this replaced.


## The Release Build

`make release` is a three-stage profile-guided build: compile instrumented, run a training
workload, then recompile with the profile and link-time optimization. It is worth about 1.5x over
the default `-O2` build on the performance suite and 1.25x on the include library test suite.

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
`bsRetain`, `bsRelease`, and the object key comparisons are called from everywhere and can
only be inlined across the library at link time.

Under PGO and LTO, `-O2` emits about 10% less text than `-O3` at the same speed. Also
measured on the same suite, and not used: `-fno-stack-protector` (3% less text, 1.6% fewer instructions, no measurable time - not
worth a mitigation), `-fomit-frame-pointer`, `-flto=thin`, and `-mcpu=native` all land within
build-to-build noise, which is about 1% between two builds of the same configuration. Disabling
the code generator's tail merging, so that every threaded-dispatch jump stays a distinct indirect
branch, measured the same way once; the release build now does so - the Makefile says why. Dead stripping has nothing left to strip after LTO with hidden visibility, and
context-sensitive PGO is not available: Apple's linker does not instrument under LTO.

A second pass went through the rest of the compiler, linker, and profile knobs. Each variant was
built on the same profile as the baseline where the flag allows it, and measured as instructions
retired and cycles over the performance suite, the include library test suite, a held-out
word-count script that no training program resembles, and an empty script for startup - against
two identical baseline builds, which differ from each other by 0.5% in cycles. Text is the
`__text` section of the shared library, 196 KB in the release build.

| Variant                                                        | Text      | Speed                    |
| -------------------------------------------------------------- | --------: | ------------------------ |
| `-O3`, re-measured on the current code                         | +6%       | noise                    |
| inline threshold 500 / 100                                     | +17% / -2% | noise / noise           |
| hot call-site threshold 6000                                   | +11%      | -0.5% instructions       |
| **profile hot cutoff 99.9%** (default 99%)                     | **+34%**  | **-1% instructions, every test** |
| profile hot cutoff 95% / 90% / 80%                             | -13% / -17% / -19% | +1.8% / +1.9% / +3.3% cycles |
| profile cold cutoff 99% (default 99.9999%)                     | -12%      | +2.3% cycles             |
| `minsize` on profile-cold functions (`-pgo-cold-func-opt`)     | -2%       | noise                    |
| `-fno-unroll-loops`                                            | -7%       | +3.3% cycles             |
| `-fno-vectorize -fno-slp-vectorize`                            | +0.4%     | +1.3% cycles             |
| hot/cold splitting                                             | -2% (+4% file) | noise               |
| function alignment 32 / no-fall-through block alignment 16    | +3% / +9% | noise                    |
| `-fno-jump-tables`, ext-TSP block placement, GVN hoisting      | 0         | noise                    |
| front-end instrumentation (`-fprofile-instr-generate`)         | +31%      | +4.6%                    |
| pre-instrumentation inlining off (`-disable-preinline`)        | -5%       | +4.7%                    |
| hot-first function order file from the profile (`-order_file`) | 0         | noise, startup included  |
| `-Wl,-no_deduplicate`, `-Wl,-ld_classic`                       | 0         | noise                    |
| the CLI and library as one statically linked binary            |           | noise, startup included  |

`-falign-loops`, `-fmerge-all-constants`, loop flattening, DFA jump threading, 32 value-profile
counters per site instead of 8, and promoting up to 8 indirect-call targets all produce the same
binary, give or take a few hundred bytes. The cold-side knobs shrink the code by de-optimizing
whatever the training touched lightly - `minsize` lands on the datetime accessors and the array
fallbacks, for one - which is the benchmark-overfitting these builds are meant to avoid, so none
is taken for a 2% saving. The one knob that moves the suite, the 99.9% hot cutoff, is `-O3`'s
trade again with a worse ratio: a third more code for 1% fewer instructions and no wall-time
change, so it is not taken either. Two mechanics worth knowing: under `-flto` an `-mllvm` option
given at compile time shapes only the pre-link pipeline and never reaches the link-time code
generator, which takes `-Wl,-mllvm,`; and the hot cutoff snaps to the profile summary's fixed
percentiles, so 99.5% builds the same binary as 99.9%.

The release also builds with `-DNDEBUG`, which is a convention rather than an effect - the
sources contain no `assert`. And clang's default `-ffp-contract=on` fuses 13 multiply-adds in
the library, all in the bitwise operators' 32-bit conversion and in radix digit accumulation,
where every operand is an exact integer below 2^53; `-ffp-contract=off` would change no result the
JavaScript and Python implementations could observe.

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

The mix itself was then varied, measured the same way and on the held-out script as well.
Training on the performance suite alone is 3% slower overall and 5% slower on the test suite;
the test suite alone is 2% slower and 4% slower on `markdownParse`; weighting the performance
suite 3:1 is 2% slower, weighting the test suite 3:1 is noise; and adding a third program that
sweeps the built-in library - JSON, sorting, strings, regular expressions, objects, numbers,
dates - grows the code 5% and moves nothing, the held-out script included. The two-program,
equal-weight mix is the optimum.

## Testing and Coverage

The unit tests print a line per test with its result and duration, then a summary in the shape
of Python's unittest report. `make test` and `make cover` accept a `TEST` variable that filters test
cases by name substring, and `QUIET=1` for a dot per test with the failure details at the end:

```sh
make test TEST=regex
make test QUIET=1
```

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
