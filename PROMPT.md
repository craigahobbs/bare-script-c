Create a pure-C version of the BareScript runtime. The build output is a dynamic library with the runtime exports and
the CLI application.

- Create a GNU Make based build system with `make test`, `make cover`, `make clean`, `make perf`, `make test-include`,
  etc. Perhaps `make compile` to compile and link.

- Create a simple unit-test system to test all code. Measure and report unit test coverage and fail the build under 100%
  coverage.

- The value system should be ref-counted and as light-weight as possible. Object should be implemented as a simple
  binary search trees. Arrays are vectors of value references.

- Determine the optimal BareScript function format. The standard library functions will be implemented in this format.

- Determine the fetchFn format. Create an optional libcurl based fetchFn.

- Create a targeted JSON encode/decode implementation for bare-script-c

- Create a targeted regular expression implementation for bare-script-c

- Use barescriptParser.bare to parse BareScript. If necessary, translate from BareScript model object value to internal
  runtime representation.

- Use barescriptLint.bare to lint BareScript

- Include the BareScript include library in the dynamic library for code-loading parser JSON. Use the exact same
  compression scheme. Use BareScript in the Makefile to generate the includeSource.c.

- Include the BareScript include library tests and the `make test-include` target (and its sub-tests)

- Provide exports for the bare-script include library stubs

- For release builds use PGO. Create an optimal set of tests for PGO.
