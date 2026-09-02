# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

bare-script-c is a pure-C (C11) implementation of the
[BareScript](https://craigahobbs.github.io/bare-script/language/) runtime. It builds a shared
library with the runtime exports plus the `bare` CLI. The only required dependency is libm;
libcurl is used for HTTP if found at build time.

`PROMPT.md` holds the original design requirements. `README.md` documents the design in depth -
read its **Design** section before making non-trivial changes.

## Commands

```sh
make commit         # the pre-commit gate: test + cover + test-include + test-language
make compile        # build build/libbarescript.{so,dylib}, build/libbarescript.a, build/bare
make test           # C unit tests
make cover          # C unit tests with line coverage; FAILS THE BUILD under 100%
make test-include   # the BareScript include library suite (1407 tests, 100% coverage)
make test-language  # this project's own BareScript language tests
make perf           # performance suite -> build/perf.csv
make release        # three-stage PGO+LTO build in build/release
make includes       # regenerate src/includeSource.c (checked in; only after lib/include changes)
```

Filtering and diagnostics:

```sh
make test TEST=regex_compile        # C tests whose name contains the substring
make cover TEST=json                # same filter, under coverage
make cover VERBOSE=1                # list every uncovered line
make test-include TEST=testSchemaParse   # filter the BareScript suite
```

Always run `make commit` before committing.

## Conformance discipline

This runtime is expected to be observably identical to the two reference implementations, which
live as sibling checkouts at `../bare-script` (JavaScript) and `../bare-script-py` (Python). `make
perf` merges their results when present.

- `make test-include` runs the *reference* test suite, vendored under `lib/include/`, and its
  output must match the JavaScript implementation's byte for byte. A diff against `bare-script` is
  therefore a conformance check on the parser, runtime, library, regex engine, linter, and CLI at
  once.
- `lib/include/*.bare` and `lib/include/test/*` are **vendored from the reference** - do not edit
  them to make a test pass; fix the C instead.
- Bundled include models are gzip-compressed by `gzip.bare` and embedded as byte arrays;
  regenerate with `make includes` after changing `lib/include/` or `bin/includeSource.bare`.
- `jsonParse` and `regexNew` messages match CPython's `json` and `re` exactly, including
  positions. Where they cannot, it is because BareScript specifies *JavaScript* regular
  expressions; those divergences are tabulated in README's **Compatibility** section. Verify
  changes here empirically against CPython rather than by inspection.
- README's **Compatibility** section records every deliberate behavioral choice where the two
  references disagree with each other. Update it when behavior changes.

## Architecture

### The parser is BareScript, not C

BareScript is parsed by `barescriptParser.bare` and linted by `barescriptLint.bare` - the same
include library scripts the reference implementations use, running on this runtime. Syntax, the
lowering of structured statements, and the exact text and column of every parse error are
inherited rather than reimplemented, so **parser bugs are usually runtime, regex, or library
bugs**.

The bootstrap: the bundled parser is stored as its own parser-compiled JSON model, so loading it
needs only a JSON decode.

```
barescriptParser.bare (bundled JSON model) --decode--> BareScript model
    --bsScriptFromModel (model.c)--> bytecode --> runs, and parses your script
```

`src/model.c` compiles the model to bytecode and keeps the original model on the script for lint
and coverage (`bsScriptToModel` retains it). Jump labels become instruction indexes and
function-local names become slot indexes during emit. A slot holding the internal unset marker
falls through to the globals object. Group nodes stay in the model and flatten only in the code
stream. The interpreter is `bsRunCode` in `src/runtime.c`.

Invariants the interpreter trusts rather than checks: the emitter computes each chunk's maximum
stack depth (`stackMax`), so pushes have no bounds checks; `LOAD_SLOT`/`STORE_SLOT` are only
emitted in function bodies, which always run with a slot array; `CALL_NAME` and `LOAD_NAME`
operands index the chunk's per-site caches (`caches[]`), which hold a pointer to the globals
object's value slot validated by the object's *structural* `generation` (bumped only when a key is
added or removed - an in-place update does not move slots). Runtime errors are checked after each
call, not per statement; line numbers come from `coverPcs` on demand. Bundled (system) includes
have their `STMT` instructions stripped by `bsScriptDropModel`, so never assume a `STMT` precedes
every statement in a cached include's code.

### The bundled include library

The include library scripts are compiled to JSON models, gzip-compressed at level 9 with
`gzip.bare`, and embedded as byte arrays in `src/includeSource.c`, which is
**generated and checked in** so a fresh clone builds with no bootstrap. `make includes`
regenerates it by running `bin/includeSource.bare` - itself a BareScript program - under a CLI
built from the *existing* generated source.

`bin/includeSource.bare` serializes objects in insertion order itself, delegating only leaf values
to `jsonStringify`.

### Values and reference counting

`BSValue` is a 16-byte tagged struct passed by value. Null, boolean, number, and datetime are
immediate; string, array, object, function, and regex point at a refcounted heap object. The rules
are uniform and are the single easiest thing to get wrong:

- A function that **returns** a `BSValue` returns an **owned** reference - the caller releases it.
- A function that **takes** a `BSValue` takes a **borrowed** reference - retain only to keep it
  past the call.
- Container accessors (`bsArrayGet`, `bsObjectGet`) return **borrowed** references.

Objects hold up to four pairs inline, then an insertion-order list of nodes indexed past 32 keys
by an interned-pointer hash table; the inline and list forms share a union. The **treap** (a BST
with a max-heap on a pseudo-random priority) over the same nodes gives the sorted traversal that
JSON encoding and value comparison are defined over, and is built lazily - only for a sorted walk
or a key that must be matched by content. Invariants: past 32 keys the hash table exists; if the
object has any uninterned key past 32 keys, the treap exists. Two distinct interned strings never
compare equal, so interned key compares are pointer compares. Strings, arrays, objects, and nodes
are recycled through free lists.

Allocation failure is fatal (`bsAlloc` aborts); do not thread out-of-memory results through value
operations.

### Library functions

```c
typedef BSValue (*BSFunctionFn)(const BSValue *args, size_t argCount, BSOptions *options, void *data);
```

`args` is a borrowed, non-allocated array (the evaluator uses an inline buffer for up to eight
arguments); the return is owned. Two distinct error paths:

- **Runtime error** (halts the script): `bsErrorSet` / `bsErrorSetStatement`.
- **Argument error** (does not halt): reported by `bsArgsValidate` from a static `BSArgModel[]`,
  which applies the same coercion and range rules as the references' `value_args_validate`, or by
  `bsFunctionError` for a failure the argument model cannot express (an unparseable JSON string,
  an invalid regular expression). The function then returns its documented error value.

### Regular expressions

`src/regex.c` compiles to a node tree and backtracks with an explicit continuation list, over
Unicode code points. It is **on the parser's hot path**, so its four performance properties are
load-bearing, not decoration: anchored-pattern optimization, iterative matching of single-code-point
quantifiers, a capture undo trail, and depth/step budgets. Syntax is the JavaScript subset
BareScript exposes - see README's **Regular Expressions** table.

## Source layout

| Path                      | Role                                                          |
| ------------------------- | ------------------------------------------------------------- |
| `include/barescript/`     | the public API; `barescript.h` includes the rest              |
| `src/value.c`             | values, refcounting, strings, arrays, the object treap        |
| `src/runtime.c`           | bytecode interpreter, includes, coverage                      |
| `src/library.c`           | the built-in library and `bsArgsValidate`                     |
| `src/model.c`             | BareScript model -> bytecode; saved model for lint/coverage   |
| `src/parser.c`            | thin wrapper running the BareScript parser and linter         |
| `src/include.c`           | system include resolution and model decompression             |
| `src/includeSource.c`     | **generated** - compressed include library models             |
| `src/bare.c`, `src/main.c`| the CLI (`bsMain`) and its entry point                        |
| `src/internal.h`          | declarations shared across implementation files               |
| `test/`                   | C unit tests; `test/include/` is the BareScript language suite |
| `lib/include/`            | **vendored** reference include library and its test suite     |
| `bin/`                    | BareScript build tools (include source generator, perf report) |
| `perf/`                   | the benchmark and a native C baseline                         |

## Tests and coverage

C unit tests self-register - adding one is a single `TEST(name) { ... }` block in an existing
`test/test_*.c` file. Assertions are the `ASSERT_*` macros in `test/test.h`; `ASSERT_VALUE`
compares a value against expected JSON. A failed assertion aborts that test and continues with the
next, so **only the first failure in each test is reported** - re-run after each fix.

Coverage is gathered with `gcov`/`llvm-cov` and summarized by `test/coverage.awk`, which honors
`GCOV_EXCL_LINE` and `GCOV_EXCL_START`/`GCOV_EXCL_STOP`. Those markers are reserved for
out-of-memory aborts, platform-specific fallbacks, and guards against a corrupted parser - prefer
deleting genuinely unreachable code or writing the test that covers it.
