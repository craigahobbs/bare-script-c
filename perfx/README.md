# perfx

`perfx` measures BareScript against other languages on real-world-like applications: an n-body
simulation (compute-heavy), web log analysis (regular expressions, hash maps), a JSON pipeline,
grid pathfinding (arrays, a binary heap), and a CSV sales report (character loops, formatting).
Each application is ported to BareScript, JavaScript (V8 with and without its JIT), Python, Lua,
Ruby, and Perl. Every port generates identical input from a shared pseudo-random sequence and
prints a result the runner checks across languages, so a timing only counts when the ports did the
same work. The runner measures each process's application time, wall time, CPU time, and peak
memory, and writes a Markdown report that explains what each column means.

```sh
make perfx                                        # the whole suite against the release build
make perfx PERFX_ARGS="--apps nbody --runs 5"     # options pass through; see perfx.py --help
make perfx-check                                  # every port at a small size: do they all agree?
```

The report is written to `build/perfx/report.md`, with every run in `results.json` and the
summary rows in `results.csv`. The other interpreters are assumed to be on the path: `node`,
`python3`, `lua`, `ruby`, `perl`.

## Findings

Measured on an Apple M3 Max with Node 26.8, Python 3.14, Lua 5.5, Ruby 2.6, and Perl 5.34, best
of three runs. Application time in milliseconds, with the fastest port of each application in
bold, and the geometric mean of each language's ratio to the fastest port across applications:

| Language                |       nbody | loganalyze |    jsonetl |   pathfind | salesreport | geomean vs fastest |
| ----------------------- | ----------: | ---------: | ---------: | ---------: | ----------: | -----------------: |
| JavaScript (V8 JIT)     | **17.5 ms** | **504 ms** | **111 ms** | **131 ms** |  **144 ms** |              1.00x |
| JavaScript (V8 jitless) |      810 ms |     996 ms |     171 ms |     736 ms |      341 ms |              4.52x |
| BareScript              |      856 ms |     720 ms |     165 ms |     816 ms |      462 ms |              4.61x |
| Python                  |      923 ms |     1.01 s |     301 ms |     771 ms |      405 ms |              5.43x |
| Lua                     |      592 ms |     1.00 s |     1.28 s |     464 ms |      617 ms |              6.53x |
| Ruby                    |      1.18 s |     1.07 s |     512 ms |     1.08 s |      1.02 s |              8.25x |
| Perl                    |      2.18 s |     1.18 s |     4.47 s |     2.80 s |      996 ms |             17.69x |

**Performance.** BareScript ties V8's bytecode interpreter and runs ahead of CPython, Lua, Ruby,
and Perl; only V8's optimizing JIT is faster. It is the fastest interpreter on the log analysis
and JSON tests, where the work runs in the C regular expression engine and JSON codec, and trails
Lua by 1.5x to 1.8x on the n-body and pathfinding tests, where every field and element access is
a library call rather than an instruction. (Lua's JSON figure is a pure-Lua codec and Perl's is
its core `JSON::PP`; neither language ships a native one.)

**Startup.** Launching the empty program:

| Language                |    Wall | Peak RSS |
| ----------------------- | ------: | -------: |
| Lua                     |  2.7 ms |   1.7 MB |
| BareScript              |  3.3 ms |   3.1 MB |
| Perl                    |  4.6 ms |   4.3 MB |
| Python                  | 16.4 ms |  14.6 MB |
| JavaScript (V8 JIT)     | 26.9 ms |  38.2 MB |
| Ruby                    | 44.1 ms |  27.6 MB |

**Memory.** BareScript has the lowest peak of any runtime on the JSON pipeline (139 MB, against
160 MB for V8 and 202 MB for Lua) and the pathfinder (33 MB), and about a third of V8's peak on
the log analysis (137 MB against 363 MB). Lua uses half as much on the sales report, since it
interns every short string and the CSV fields repeat. BareScript is single-threaded, so its CPU
time equals its wall time; V8 spends up to a third more CPU than wall on background compilation and
garbage collection.
