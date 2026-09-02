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
- Bundled include models are gzip-compressed by `gzip.bare` and base64-encoded by `base64.bare`;
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
    --bsScriptFromModel (model.c)--> compiled script --> runs, and parses your script
```

`src/model.c` converts a BareScript model object into the runtime's compiled representation and
back (`bsScriptToModel`, which is how the linter receives a script). Two passes run once a
statement list is complete: **jump resolution** (label -> statement index) and **slot resolution**
(a function's locals -> array indexes, with an unset marker falling through to globals).

### The bundled include library

The include library scripts are compiled to JSON models, gzip-compressed at level 9 with
`gzip.bare`, base64-encoded with `base64.bare`, and embedded in `src/includeSource.c`, which is
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

Objects are **treaps** (a BST with a max-heap on a pseudo-random priority) threaded on a
doubly-linked insertion-order list: the tree gives the sorted traversal that JSON encoding and
value comparison are defined over, while iteration stays in insertion order to match the
references. The treap matters because BareScript code routinely inserts keys in sorted order.

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
| `src/runtime.c`           | the statement loop and expression evaluator                   |
| `src/library.c`           | the built-in library and `bsArgsValidate`                     |
| `src/model.c`             | BareScript model <-> compiled script, jump and slot resolution |
| `src/parser.c`            | thin wrapper running the BareScript parser and linter         |
| `src/include.c`           | system include resolution and model decompression             |
| `src/includeSource.c`     | **generated** - compressed include library models             |
| `src/bare.c`, `src/main.c`| the CLI (`bsMain`) and its entry point                        |
| `src/internal.h`          | declarations shared across implementation files               |
| `test/`                   | C unit tests; `test/include/` is the BareScript language suite |
| `lib/include/`            | **vendored** reference include library and its test suite     |
| `bin/`                    | BareScript build tools (include source generator, perf report) |
| `perf/`                   | the benchmark, a native C baseline, and the PGO training script |

## Tests and coverage

C unit tests self-register - adding one is a single `TEST(name) { ... }` block in an existing
`test/test_*.c` file. Assertions are the `ASSERT_*` macros in `test/test.h`; `ASSERT_VALUE`
compares a value against expected JSON. A failed assertion aborts that test and continues with the
next, so **only the first failure in each test is reported** - re-run after each fix.

Coverage is gathered with `gcov`/`llvm-cov` and summarized by `test/coverage.awk`, which honors
`GCOV_EXCL_LINE` and `GCOV_EXCL_START`/`GCOV_EXCL_STOP`. Those markers are reserved for
out-of-memory aborts, platform-specific fallbacks, and guards against a corrupted parser - prefer
deleting genuinely unreachable code or writing the test that covers it.
