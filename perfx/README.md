# perfx

`perfx` is a cross-language performance testing framework. It runs a set of real-world-like
applications, each ported line for line to several languages, under one runner that measures every
process the same way and writes a report built to explain the numbers rather than just list them.

```sh
make perfx                                        # the whole suite, best of three -> build/perfx/report.md
make perfx PERFX_ARGS="--apps nbody --runs 5"     # options pass through
make perfx-check                                  # every port at a small size: do they all agree?
python3 perfx/perfx.py --help                     # the runner directly
```

The runner needs the release build of `bare` (`make release`, or `--bare PATH`) and assumes the
other interpreters are installed and on the path: `node`, `python3`, `lua`, `ruby`, `perl`.

The results from this suite on one machine are summarized under **Performance** in the top-level
[README](../README.md#performance).

## The applications

Every application is a small program a working developer might actually write, not a
micro-benchmark of one operation. Each generates its own input from a deterministic
pseudo-random generator - the same Park-Miller sequence in every language - so every port
processes identical data, and each prints a result that the runner checks across ports.

| Application   | Workload `n`  | What it does                                                         | What it exercises                                              |
| ------------- | ------------- | -------------------------------------------------------------------- | -------------------------------------------------------------- |
| `nbody`       | 300,000 steps | Advances the five-body Jovian planet system with a symplectic integrator and reports its energy | Floating-point arithmetic, field access on records; **the compute-heavy test** |
| `loganalyze`  | 300,000 lines | Generates an Apache-style access log, parses every line with a regular expression, aggregates clients, status codes, hourly traffic, and bytes per path | Regular expressions, string building and splitting, hash maps, sorting |
| `jsonetl`     | 50,000 orders | Builds nested customer orders, serializes them to JSON, parses the text back, ranks customers and SKUs | JSON encoding and decoding, nested containers, grouping |
| `pathfind`    | 800 x 800 grid | Runs Dijkstra's algorithm corner to corner across a weighted grid with a fifth of the cells blocked, with a hand-written binary heap | Arrays, integer arithmetic, a priority queue |
| `salesreport` | 100,000 rows  | Generates CSV sales records with a quoted field, parses them with a character-level CSV reader, computes per-region statistics, renders an aligned money-formatted report | Character loops, string formatting, statistics |

A sixth "application", `empty`, prints its result and exits. It measures each interpreter's
startup time and baseline memory, which the report subtracts from the application figures.

The workload sizes are chosen so the slowest ports take a few seconds and the fastest still
measure well above clock resolution. `--scale` multiplies them (`--quick` is one run at a fifth;
`--check` is a verification-only run at a fiftieth).

## The languages

| Key      | Language                  | Command                        |
| -------- | ------------------------- | ------------------------------ |
| `bare`   | BareScript                | `bare app.bare -v vN n`        |
| `js`     | JavaScript (V8 JIT)       | `node app.js n`                |
| `jsless` | JavaScript (V8 jitless)   | `node --jitless app.js n`      |
| `py`     | Python                    | `python3 app.py n`             |
| `lua`    | Lua                       | `lua app.lua n`                |
| `rb`     | Ruby                      | `ruby app.rb n`                |
| `pl`     | Perl                      | `perl app.pl n`                |

`--langs` selects a subset by key. The two JavaScript rows run the same source: with V8's
optimizing compilers, and with them disabled so V8's bytecode interpreter runs the program - the
closest thing to an apples-to-apples comparison with the other interpreters.

## The port protocol

A port is one source file, `apps/<app>/<app>.<ext>`, that:

1. Takes the workload size `n` as its first argument (BareScript reads the `vN` global the CLI sets
   with `-v`), defaulting to the size in the table above.
2. Does all of its work - input generation included - between two readings of a monotonic clock.
3. Prints exactly two lines: `result: <text>` and `time: <milliseconds>`.

The result is an integer or a short string built from integers, never a formatted float, so every
language prints it identically. Floating-point work is folded into the result through `floor` of a
scaled value, and text output is folded through a 32-bit string hash. The runner takes the most
common result across the ports as the expected one and flags any port that differs or fails.

Ports use each language's ordinary idioms - dictionaries, classes or records, native regular
expressions, the standard JSON library - rather than a lowest-common-denominator style, because the
question is what a program written naturally in the language costs. Two exceptions are documented in
the report: Lua has no JSON in its standard library, so its `jsonetl` port carries the small
pure-Lua codec a Lua program would vendor, and Perl's core `JSON::PP` is written in Perl. Lua also
has no sub-second wall clock in its standard library, so its app time is CPU time from `os.clock`,
which is the same figure for a single-threaded program.

To add an application, create its directory with a port per language and add an entry to `APPS`
in `perfx.py` (name, default size, whether the work grows linearly or quadratically with the size,
and a description). To add a language, add an entry to `LANGUAGES` with its command line and port
every application. `make perfx-check` then confirms the new ports agree with the rest.

## Measurement

The runner spawns each process itself and collects its resource usage with `wait4`, so every
figure is per process and per run:

- **App time** from the port's own `time:` line - the work, without startup or teardown.
- **Wall time** from spawn to exit, as a shell would see it.
- **User and system CPU time** from the kernel's accounting for that process.
- **Peak RSS**, the largest resident set size the process reached.

Runs are interleaved by language - round one of every port, then round two - so a drifting
machine affects the languages alike. Each figure in the report is the best of the runs, which is
the least-disturbed measurement; peak RSS is the largest seen, since memory is about the worst
case. `results.json` keeps every individual run and `results.csv` the summarized rows for further
processing.

## The report

`build/perfx/report.md` has five parts:

- **Interpreters** - the exact command and version behind every row, so a report is reproducible.
- **Summary** - application time per language and application, with the fastest port in bold, and
  each language's geometric mean of its ratio to the fastest port across applications. The geometric
  mean weights every application equally, so one lopsided test cannot dominate the ranking. An ASCII
  bar chart shows the same ratios at a glance.
- **Startup** - the empty program's wall time, CPU time, and peak RSS per interpreter: the fixed
  cost of launching each runtime, and the memory floor the application tables subtract.
- **Applications** - one table per application: app time, the ratio to the fastest port, wall time,
  wall minus app (startup, source loading, teardown), user and system CPU, peak RSS, and RSS above
  the interpreter's baseline. Below each table is the shared result, or the ports that failed to
  match it.
- **Memory** - peak RSS per language and application in one table, next to each baseline.

The closing section explains how to read each column; in short, compare *app time* to rank the
languages on the work itself, *wall minus app* and the *Startup* table for what a short-lived
script pays before it starts, *user CPU above wall* to see which runtimes spread work over
background threads, and *above baseline* to see what the application's data cost on each runtime.
