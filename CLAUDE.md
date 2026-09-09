# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

bare-script-c is a pure-C (C11) implementation of the
[BareScript](https://craigahobbs.github.io/bare-script/language/) runtime. It builds a shared
library with the runtime exports plus the `bare` CLI. The only required dependency is libm;
libcurl is used for HTTP if found at build time.

`PROMPT.md` holds the original design requirements. `DESIGN.md` documents the design in depth -
read it before making non-trivial changes. `README.md` is for users: building, the command line,
embedding, and the measurements.

## Commands

```sh
make commit         # the pre-commit gate: test + cover + test-include + test-language + release
make compile        # build build/libbarescript.{so,dylib}, build/libbarescript.a, build/bare
make test           # C unit tests
make cover          # C unit tests with line coverage; FAILS THE BUILD under 100%
make test-include   # the BareScript include library suite (1407 tests, 100% coverage)
make test-language  # this project's own BareScript language tests
make perf           # performance suite -> build/perf.csv
make perf PERF_MERGE= PERF_RUNS=5   # this runtime only (no sibling suites), best of five
make perfx          # cross-language application suite -> build/perfx/report.md (see perfx/README.md)
make perfx-check    # verify every perfx port computes the same result
make release        # three-stage PGO+LTO build in build/release
make includes       # regenerate src/includeSource.c (checked in; only after lib/include changes)
make release INCLUDE="barescriptParser.bare barescriptLint.bare url.bare"  # bundle only these includes
make release INCLUDE_EXCLUDE="qrcode.bare draw.bare"                         # bundle all but these
```

Filtering and diagnostics:

```sh
make test TEST=regex_compile        # C tests whose name contains the substring
make cover TEST=json                # same filter, under coverage
make test QUIET=1                   # a dot per test instead of a line per test
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
- This implementation defines BareScript's behavior; agreement between the two references is
  evidence, not authority, and a behavior change is the user's decision. Regular expressions and
  Unicode whitespace and case behavior are defined as standard JavaScript's. The other ports are
  defined as the same as this implementation, within reason; README's **Compatibility** section
  records the definitions and the ports' deviations.

## Architecture

### The parser is BareScript, not C

BareScript is parsed by `barescriptParser.bare` and linted by `barescriptLint.bare` - the same
include library scripts the reference implementations use, running on this runtime. Syntax, the
lowering of structured statements, and the exact text and column of every parse error are
inherited rather than reimplemented, so **parser bugs are usually runtime, regex, or library
bugs**.

The bootstrap: the bundled parser is stored as its own parser-compiled model in a binary encoding
(`bin/includeSource.bare` describes it), so loading it needs no parser: `bsScriptFromModelBinary`
reads the bytes straight into the emitter's transient syntax tree a statement at a time and builds
no model objects at all (DESIGN.md's **The Parser and Linter** draws it).

`src/model.c` compiles a statement from a transient syntax tree (`BSAst`, an arena of nodes) to
bytecode. A parsed script's model objects are loaded into the tree a statement at a time by
`bsAstStatement`; a bundled include's binary model is read into it by `bsScriptFromModelBinary`,
and a system include's JSON model is decoded to objects first. The
loaders reject a malformed model; the emitter assumes a well-formed tree. A script keeps its model
only where something will read it - the CLI under static analysis, an include while coverage is
recording; otherwise `bsScriptForgetModel` drops it and `bsScriptToModel` re-parses the retained
source lines on demand. Instructions are eight-byte register instructions - a destination register and two
operand registers. A chunk's registers are its slots (a function's arguments and assigned names),
then the temporaries the emitter allocates stack-fashion while compiling an expression, then its
constants, copied in when the frame is built, so a local or a literal feeds an operator or a call
with no instruction of its own and no operand tag test; the names a call, load, or store site
refers to live in a table of their own. Jump labels become instruction indexes during emit. A slot holding the internal unset
marker falls through to the globals object: the emitter's definite-assignment analysis (a must-
analysis over each function body's labels and jumps) reads a slot that is definitely assigned as a
plain register operand, and one that might be unset through `LOAD_SLOT`, which tests. Group nodes
stay in the model and flatten only in the code stream. The interpreter is `bsRunCode` in
`src/runtime.c`.

Invariants the interpreter trusts rather than checks: the emitter counts each chunk's temporaries
(`tempCount`) and every chunk arrives with its registers filled, so register operands are never
bounds-checked, and a register operand is never the unset marker (only `LOAD_SLOT` reads a slot
that might be); a call's argument operands sit in the `DATA` words that follow it and are read
into a borrowed argument array; `CALL_NAME`, `LOAD_NAME`, and `STORE_NAME` operands index the
chunk's per-site caches (`caches[]`), which hold a pointer to the globals
object's value slot validated by the object's *structural* `generation` (bumped only when a key is
added or removed - an in-place update does not move slots); an `objectGet` or `objectSet` site's
cache also remembers the entry index its key was last found at, checked against the entry's key
before the object is scanned. Runtime errors are checked after each
call, not per statement; line numbers come from `coverPcs` on demand. A system script - a bundled
include - is emitted without `STMT` instructions, so never assume a `STMT` precedes every
statement in a cached include's code.

### The bundled include library

`src/includeSource.c` is **generated and checked in** so a fresh clone builds with no bootstrap;
`make includes` regenerates it by running `bin/includeSource.bare` - itself a BareScript program -
under a CLI built from the *existing* generated source. DESIGN.md's **The Bundled Include Library**
describes the encoding. Any include but the parser and linter compiles out under its
`NO_BARESCRIPT_INCLUDE_<NAME>` macro; the Makefile's `INCLUDE` list sets the macro for every include
not named, and its `INCLUDE_EXCLUDE` list for every include named. There is no dependency tracking,
so an include's own includes must be listed with it.

### Values and reference counting

`BSValue` is a 16-byte tagged struct passed by value; DESIGN.md's **The Value System** gives the
layout and the ownership rules (returns are owned, arguments and container accessors are borrowed),
which are uniform and the single easiest thing to get wrong.

Object invariants the code relies on: an object is one insertion-ordered entry array - in the
object itself up to three entries, on a heap buffer past that - and past sixteen keys it has a hash
index over the entries, keyed by the content hash cached on the key string; a value-slot pointer
is valid until a key is added or removed. Two distinct interned strings never compare equal, so
interned key compares are pointer compares and the bytes are compared only when the two keys are
not both interned. Strings, arrays, objects, and entry buffers are recycled through free lists.

Allocation failure is fatal (`bsAlloc` aborts); do not thread out-of-memory results through value
operations.

All mutable runtime state is `_Thread_local` - the free lists, the intern table, the model keys, the
library function values, the compiled parser, linter, and include caches, the system include
registry, the RNG - so each thread is an isolated runtime, and values, scripts, and options never
cross threads (DESIGN.md's **Threads**). Keep it so: a new file-scope variable is `_Thread_local` or
`const`. The one process-wide object is the libcurl loader, behind a C11 atomic once. The cleanups
are per thread, `bsValueCleanup` last; `test/test_thread.c` runs eight runtimes at once.

### Library functions

The function format is DESIGN.md's **The Function Format**: `args` is a borrowed slice of the
interpreter's borrowed reads of the call's operands, the return is owned. Two distinct error paths:

- **Runtime error** (halts the script): `bsErrorSet` / `bsErrorSetStatement`.
- **Argument error** (does not halt): reported by `bsArgsValidate` from a static `BSArgModel[]`,
  which applies the same coercion and range rules as the references' `value_args_validate`, or by
  `bsFunctionError` for a failure the argument model cannot express (an unparseable JSON string,
  an invalid regular expression). The function then returns its documented error value.

The library's functions open with the `BS_ARGS(model, failValue)` macro. Functions with an
`intrinsic` id are handled by the interpreter on their happy-path argument shapes; a miss falls
through to the function itself. The single-shape intrinsics (`arrayGet`, `arrayLength`, `arrayPush`,
`arraySet`, `objectGet`, `objectHas`, `objectSet`, `stringCharCodeAt`, `stringLength`, `stringSlice`,
and the one-argument math functions, which share one opcode) compile to call opcodes of their own
(`bsCallOpcode` in `src/model.c`, the `bsIntrin*` functions in `src/runtime.c`), guarded by the
site's *warm* cache - valid for the activation's globals object, its global carrying that intrinsic
id - with a cold site, or one an expression's locals object could shadow, taking the general call,
which resolves the cache; they have no case in `bsIntrinsicCall`, which serves the rest through the
general call path.

### Regular expressions

`src/regex.c` parses to a node tree, compiles the tree to a linear program, frees the tree, and
matches by running the program in one loop with an explicit backtrack stack, over Unicode code
points. It is **on the parser's hot path**, so its performance properties are load-bearing, not
decoration: anchored-pattern optimization, alternations indexed by first code point (a byte or
a word of alternative bits per ASCII code point; a predefined class contributes its ASCII
members), the start-position scan by
memchr or a byte table over an ASCII subject, single-code-point quantifiers that scan in one loop and give back through one
backtrack entry, a capture and counter undo trail, and a step budget. Syntax is the JavaScript
subset BareScript exposes - see DESIGN.md's **Regular Expressions** table.

## Source layout

| Path                      | Role                                                          |
| ------------------------- | ------------------------------------------------------------- |
| `include/barescript/`     | the public API; `barescript.h` includes the rest              |
| `src/value.c`             | values, refcounting, strings, arrays, objects, the intern table |
| `src/runtime.c`           | bytecode interpreter, includes, coverage                      |
| `src/library.c`           | the built-in library and `bsArgsValidate`                     |
| `src/model.c`             | BareScript model -> bytecode; saved model for lint/coverage   |
| `src/parser.c`            | thin wrapper running the BareScript parser and linter         |
| `src/include.c`           | the bundled include registry and model decompression          |
| `src/json.c`              | JSON encode and decode                                        |
| `src/regex.c`             | the regular expression compiler and matcher                   |
| `src/unicode.c`           | Unicode case mapping - stringUpper, stringLower, the regex `i` flag - and its Unicode 16.0 tables |
| `src/options.c`           | the fetch, log, and URL option implementations                |
| `src/includeSource.c`     | **generated** - compressed include library models             |
| `src/bare.c`, `src/main.c`| the CLI (`bsMain`) and its entry point                        |
| `src/internal.h`          | declarations shared across implementation files               |
| `test/`                   | C unit tests; `test/include/` is the BareScript language suite |
| `lib/include/`            | **vendored** reference include library and its test suite     |
| `bin/`                    | BareScript build tools (include source generator, perf report)   |
| `perf/`                   | the benchmark and a native C baseline                         |
| `perfx/`                  | the cross-language application suite: runner, ports, report   |

## Tests and coverage

C unit tests self-register - adding one is a single `TEST(name) { ... }` block in an existing
`test/test_*.c` file. Assertions are the `ASSERT_*` macros in `test/test.h`; `ASSERT_VALUE`
compares a value against expected JSON. A failed assertion aborts that test and continues with the
next, so **only the first failure in each test is reported** - re-run after each fix.

Coverage is gathered with `gcov`/`llvm-cov` and summarized by `test/coverage.awk`, which honors
`GCOV_EXCL_LINE` and `GCOV_EXCL_START`/`GCOV_EXCL_STOP`. Those markers are reserved for
out-of-memory aborts, platform-specific fallbacks, and guards against a corrupted parser - prefer
deleting genuinely unreachable code or writing the test that covers it.

## Performance work

Three recurring jobs share one measurement gate. Every figure comes from the **release** build,
and nothing is committed on the strength of `make test` alone.

### The measurement gate

Before a pass, snapshot the baseline: copy `build/release/bare` and
`build/release/libbarescript.dylib` to a scratch directory (`q0`), and record the four suite
outputs -
`bare -d -m lib/include/test/runTests.bare`, `bare -d -v vUnittestReport true
lib/include/test/runTestsMarkdownUp.bare`, `bare -d -m test/include/runTests.bare`, and
`bare -x -m lib/include/*.bare lib/include/test/test*.bare`. Then for each candidate change:

1. `make test` while iterating; then the gate, chained with `if`, never `;` - a `;` chain once
   masked three failing gates: `if make commit QUIET=1 > gate.log 2>&1; then ...; else grep -n
   FAIL gate.log; fi`. Read the `OK`/`FAILED` line, not the tail.
2. Snapshot the new release next to the others and diff its four suite outputs against the
   baseline's, ignoring the `executed in` line. They must be byte-identical; a difference is a
   bug, not a new baseline.
3. Performance: run each `perf/test.bare` test (`-v vTest "'name'"`), the include suite, and an
   empty script as separate processes under `/usr/bin/time -l`, three rounds interleaving the
   baseline and the candidate, and compare the minimum **instructions retired**, cycles, and
   maximum RSS. Instructions are stable to about ±0.3%; wall time drifts 5-10% across a day, so it
   is never the deciding metric. Under `-d`, the suite's own `executed in` line is what a reader
   compares by hand.
4. Size: `size -m build/release/libbarescript.dylib` for `__text` (code) and `__const` (the
   compressed includes). When `__text` moves more than a few hundred bytes, find out why:
   `nm -n` both libraries, difference adjacent symbol addresses for per-function sizes, and join
   the two lists. Slimming a function can make LTO inline it at every hot call site and grow the
   binary by kilobytes (a 3-line trim of `bsObjectKeyIs` cost 1.8 KB, a 7-line rewrite of
   `bsObjectSetString` 7.3 KB); the fix was to keep the larger body.
5. Memory: `/usr/bin/time -l` on the empty script (maximum resident set and peak footprint), the
   perf tests, and the suite plain (`-d`) and with coverage (`-d -m`).

Keep every gated snapshot for the day; they bisect a reported regression in seconds. Commit each
verified change on its own, with a one-line imperative message. Revert anything that is not
clearly better on the axis it targets and neutral on the others; note what was tried and measured
neutral so it is not retried.

### The simplification loop

A full-code review whose only aims are reducing code, improving consistency, and improving
clarity - nothing else. Performance, memory, and binary size belong to the profile-driven loop.
Then each worthwhile idea implemented and validated on its own - the gate, the suite diff, and
before/after performance, memory, and size so a simplification does not regress them - and
committed if it holds up or reverted if not, until the ideas run out:

- Review the whole codebase, not the diff: every source file group (value/json/options/include;
  model/library/runtime/regex; parser/bare/tests), the headers, and the tests. A repeated-window
  scan (normalized six-line windows across `src/*.c`) and an unused-declaration scan (each name in
  `internal.h` and the public headers counted across `src/`) find what reading misses.
- A candidate is fewer lines, more consistent form, or clearer expression of the same behavior:
  a shared helper for a repeated sequence, an unread field or parameter, a special case a general
  path already covers, a flag that restates state another value carries, names and control flow
  that match their neighbors. Prefer deleting unreachable code to excluding it from coverage. A
  change whose purpose is fewer instructions, less memory, or a smaller binary is not a
  candidate here.
- Apply in per-file batches, gate each batch, and measure. A candidate that trades an invariant
  the code relies on for a few lines is not one - trace what a change to a container's shape
  lets later operations assume before taking it. Inside `bsRunCode`, `rxRun`, or the emitter's
  dispatch, keep a simplification only when the gate's numbers stay neutral: more lines for the
  same behavior, or a regression, is not a keep.
- Threaded dispatch gotcha: a `}` after a threaded jump (`BS_NEXT()`, `RX_NEXT()`) is a line
  coverage never reaches; keep the label and `goto dispatch` form that leaves no such brace.
- A refcount change gets `leaks --atExit -- build/bare -c '...'` on the dev build as well as the
  gate; a leak is invisible to every test.

### The profile-driven optimization loop

Profile first, change what the profile names, measure, keep or revert:

- Build a symbolized release without LTO so `sample` sees real frames: `make release
  RELEASE_DIR=build/relg PROFILE_DIR=build/relg/profile RELEASE_CFLAGS="-O2 -DNDEBUG -g"`, then
  run the workload (`build/relg/bare -d -m lib/include/test/runTests.bare` in a loop, or one perf
  test) and `sample $PID 3 1 -mayDie -file out.txt`; aggregate self time per function (a node's
  count minus its children's). `sample` truncates deep stacks in the middle, so parser-recursion
  samples show call-site lines as leaves: function-level self time is usable, line-level inside
  `bsRunCode` is not. A `-fno-inline-functions` variant separates the call path; the LTO release
  inlines `bsNull`/`bsRetain` completely, so frames for those in a profile are artifacts.
- For counts rather than time, run the instrumented stage-2 build with `LLVM_PROFILE_FILE` set,
  merge, and `llvm-profdata show -all-functions -counts`; the first "Block counts" entry is not
  the entry count. Startup is profiled in-process: a harness that calls `bsMain` in a loop.
- Take candidates from the top of the self-time list, one at a time. A keep needs a clear win in
  instructions or cycles on the workload it targets with no regression elsewhere and no size
  growth it cannot justify; an idea that moves nothing is reverted the same hour.
- Measured neutral, do not retry as-is: PGO training mixes and `-O3`/inline-threshold flags; a
  fused call instruction that loads simple arguments; an intrinsic express lane in the call path
  (it did not replicate); a lazily built lookup table for large objects; caching the coverage
  slot pointer per `bsRunCode` entry; a JSON needs-escape table; larger array free-list classes;
  allocation-free object comparison; copying a function's constants into its frame with a loop
  instead of memcpy (schemaValidate +4.8%, the parser tests +1%); changing code the interpreter loop
  inlines - the array buffer pool, the intrinsic switch's bodies - without keeping the changed
  bodies out of line (nbody moved +0.7..1.5% on unrelated layout churn until they were `BS_NOINLINE`);
  an `objectNew` call opcode even with its body out of line (markdownElements +1.2%, nbody +1.5% -
  the general path's intrinsic switch is already as cheap for a variable-length argument list).
- The perfx suite (`make perfx`) is the yardstick for container-heavy code - `nbody` and
  `pathfind` spend their time in `objectGet`/`arrayGet` calls - where the include suite is
  parser-bound.

### Updating README's Performance section

The section has three parts - the cross-language perfx benchmarks, the include library benchmarks
with the suite time as a column, and memory and size - and every number in it is re-measured, not
edited:

1. `make perf PERF_RUNS=5` re-runs this runtime and, when the sibling checkouts are present, the
   JavaScript and Python suites; `PERF_MERGE=` skips the siblings when only this runtime changed,
   and their columns stay as they were.
2. The include suite wall time, best of five, as `{ /usr/bin/time -p build/release/bare -d -m
   lib/include/test/runTests.bare > /dev/null; } 2>&1 | awk '/^real/'`; redirecting stderr inside
   the braces swallows the timer's report. The sibling rows come from `node bin/bare.js` in
   `../bare-script` and the venv `bare` in `../bare-script-py` (pure Python needs
   `BARESCRIPT_RUNTIME_PY=1`), and stay as they were when only this runtime changed.
3. `make perfx` (best of three) for the Across Languages table, the startup table, and the memory
   paragraph; `/usr/bin/time -l` for the empty script and suite figures; `ls -l` and `size -m` on
   the release library for the sizes.
4. Tables follow the perfx report's layout: languages as rows sorted by score, tests as columns,
   the fastest per column in bold, times as `s`/`ms` (no decimals from 100 s and from 100 ms, one
   from 10 s and below 100 ms, two from 1 s), and a final `vs best` column. The score is the one
   `scores()` in `perfx/perfx.py` computes: the language effect of a multiplicative model fitted
   by least squares on the log scale, relative to the best language - the plain geometric mean
   of ratios when the table is complete, and unbiased by missing cells when it is not (the
   include library table has them: no native `qrcodeMatrix` or `testSuite`, no Python markdown).
   That table shows six representative columns (`mandelbrot`, `mdElements`, `mdParse`,
   `schValidate`, `urlEncode`, and `testSuite`, the suite's wall time scaled to the per-1000-run
   unit) but its score covers all nine tests. Generate the cells with a script that calls
   `scores()` rather than by hand.
5. `markdownParse` parses this README, so its figure moves when the file changes: measure it last,
   after the edits, and update its cells and the means it feeds.

The gate runs before the commit even for a documentation-only change.

