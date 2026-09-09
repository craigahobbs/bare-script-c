# Licensed under the MIT License
# https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE


.DEFAULT_GOAL := help


# Project
LIB_NAME := barescript
CLI_NAME := bare


# Directories
SRC_DIR := src
INC_DIR := include
TEST_DIR := test
PERF_DIR := perf
BUILD_DIR := build


# Platform
UNAME_S := $(shell uname -s)
ifeq '$(UNAME_S)' 'Darwin'
    SO_EXT := dylib
    SO_LDFLAGS := -dynamiclib -install_name @rpath/lib$(LIB_NAME).dylib
    RPATH_FLAGS := -Wl,-rpath,@executable_path -Wl,-rpath,@executable_path/../lib
else
    SO_EXT := so
    SO_LDFLAGS := -shared -Wl,-soname,lib$(LIB_NAME).so
    RPATH_FLAGS := -Wl,-rpath,'$$ORIGIN' -Wl,-rpath,'$$ORIGIN/../lib'
endif


# Toolchain
CC ?= cc
CC_IS_CLANG := $(shell $(CC) --version 2>/dev/null | grep -qi clang && echo 1)

# Use a flag only where the toolchain accepts it - the argument must not contain a comma
CC_SUPPORTS = $(shell echo 'int main(void){return 0;}' | \
    $(CC) -Werror $(1) -x c - -o /dev/null > /dev/null 2>&1 && echo $(1))

# GCC 14 warns that the test runner's setjmp/longjmp pattern clobbers a local, which -Werror makes
# an error; clang has no such option and rejects it as unknown, so the probe leaves it out there.
WARN_FLAGS := -Wall -Wextra -Werror -Wno-unused-parameter -Wshadow -Wpointer-arith \
    -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes -Wwrite-strings \
    $(call CC_SUPPORTS,-Wno-clobbered)

# No caller reads errno after a libm call. Without -fno-math-errno, GCC and clang on Linux wrap
# every sqrt, floor, and fmod in an errno check that keeps them out of line; Apple's clang already
# assumes it, so this makes the ELF build match the code generation that was measured.
BASE_CFLAGS := -std=c11 -pedantic -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -I$(INC_DIR) \
    $(WARN_FLAGS) -fno-math-errno
OPT_CFLAGS ?= -O2 -g
LIBS := -lm
TEST_LIBS := -pthread

# Shared library code generation
#
# Without this, a call from one translation unit of the library to another goes through the PLT on
# ELF targets, because the symbol could be interposed at load time - which also blocks inlining
# across the library. Nothing here is meant to be interposed. Mach-O binds these calls directly
# already, and Apple's clang rejects the flag, so the probe leaves it out there.
SO_CFLAGS := -fPIC -fvisibility=hidden $(call CC_SUPPORTS,-fno-semantic-interposition)


# Optional libcurl support for the HTTP fetch function. Only the headers are needed at build
# time - the library is loaded with dlopen on the first HTTP fetch, so a process that never fetches
# a URL never maps libcurl and its dependencies.
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null || curl-config --cflags 2>/dev/null)
CURL_LIBS := $(shell pkg-config --libs libcurl 2>/dev/null || curl-config --libs 2>/dev/null)
ifneq '$(strip $(CURL_LIBS))' ''
    BASE_CFLAGS += -DBARESCRIPT_CURL $(CURL_CFLAGS)
    ifneq '$(UNAME_S)' 'Darwin'
        LIBS += -ldl
    endif
endif


# The bundled BareScript include library
INCLUDE_LIB_DIR := lib/include
INCLUDE_LIB_SRCS := $(sort $(wildcard $(INCLUDE_LIB_DIR)/*.bare))
INCLUDE_TEST_DIR := $(INCLUDE_LIB_DIR)/test
INCLUDE_TEST_SRCS := $(sort $(wildcard $(INCLUDE_TEST_DIR)/*.bare))
INCLUDE_SOURCE_C := $(SRC_DIR)/includeSource.c
INCLUDE_SOURCE_H := $(INC_DIR)/barescript/includeSource.h

# The includes to bundle - all of them, or INCLUDE's list, or all but INCLUDE_EXCLUDE's; the parser
# and linter are bundled either way, since the runtime parses with them:
#
#   make release INCLUDE="url.bare"
#   make release INCLUDE_EXCLUDE="qrcode.bare draw.bare"
#
# Every other include is compiled out by its NO_BARESCRIPT_INCLUDE_<NAME> macro, which only
# src/includeSource.c reads - so a change to either needs a "make clean" first. For a library built
# for an application only: the test suites and the release build's training need every include.
INCLUDE ?=
INCLUDE_EXCLUDE ?=
INCLUDE_NAMES := $(notdir $(INCLUDE_LIB_SRCS))
INCLUDE_UNKNOWN := $(filter-out $(INCLUDE_NAMES),$(INCLUDE) $(INCLUDE_EXCLUDE))
ifneq '$(INCLUDE_UNKNOWN)' ''
    $(error not in $(INCLUDE_LIB_DIR): $(INCLUDE_UNKNOWN))
endif
INCLUDE_OUT := $(basename $(if $(strip $(INCLUDE)),$(filter-out $(INCLUDE),$(INCLUDE_NAMES)),$(INCLUDE_EXCLUDE)))
BASE_CFLAGS += $(patsubst %,-DNO_BARESCRIPT_INCLUDE_%,$(shell echo '$(INCLUDE_OUT)' | tr '[:lower:]' '[:upper:]'))


# Sources
CLI_SRCS := $(SRC_DIR)/bare.c
LIB_SRCS := $(filter-out $(CLI_SRCS),$(sort $(wildcard $(SRC_DIR)/*.c)))
TEST_SRCS := $(sort $(wildcard $(TEST_DIR)/*.c))


# Object files
OBJ_DIR := $(BUILD_DIR)/obj
COVER_DIR := $(BUILD_DIR)/cover
LIB_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(LIB_SRCS))
CLI_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(CLI_SRCS))
TEST_OBJS := $(patsubst $(TEST_DIR)/%.c,$(OBJ_DIR)/test-%.o,$(TEST_SRCS))
COVER_SRCS := $(LIB_SRCS) $(SRC_DIR)/bare.c
COVER_OBJS := $(patsubst $(SRC_DIR)/%.c,$(COVER_DIR)/%.o,$(COVER_SRCS)) $(TEST_OBJS)


# Build outputs
LIB_SO := $(BUILD_DIR)/lib$(LIB_NAME).$(SO_EXT)
LIB_A := $(BUILD_DIR)/lib$(LIB_NAME).a
CLI_BIN := $(BUILD_DIR)/$(CLI_NAME)
TEST_BIN := $(BUILD_DIR)/$(CLI_NAME)-test
COVER_BIN := $(BUILD_DIR)/$(CLI_NAME)-cover


#
# The pre-commit gate
#
# Everything that must pass before a commit, as it is in the JavaScript and Python
# implementations: the unit tests under coverage, the include library suite - which includes its
# static analysis run - and this project's own language tests. The release build runs too, and
# then those suites run again against it, so a flag, LTO, or profile problem cannot reach a commit
# unnoticed. Under "make -j" every suite runs as soon as the binary it tests is linked, alongside
# the release build.
#

.PHONY: commit
commit: test cover test-include test-language release test-release


.PHONY: help
help:
	@echo "usage: make [commit|compile|test|cover|test-include|test-language|test-release|perf|perfx|perfx-check|release|includes|install|clean]"
	@echo
	@echo "  commit        everything that must pass before a commit"
	@echo "  compile       build the shared library and the command-line interface"
	@echo "  test          build and run the unit tests"
	@echo "  cover         run the unit tests and report coverage (fails under 100%)"
	@echo "                VERBOSE=1 lists each uncovered line"
	@echo "  test-include  run the BareScript include library test suite"
	@echo "  test-language run this project's own BareScript language tests"
	@echo "  test-release  run those suites again against the release build"
	@echo "                test-release-{lint,markdownup,run,language} run one of them"
	@echo "  perf          run the performance suite against the release build"
	@echo "  perfx         run the cross-language application suite against the release build"
	@echo "  perfx-check   verify that every perfx port computes the same result"
	@echo "  release       profile-guided optimization build in build/release"
	@echo "  includes      regenerate the bundled include library source"
	@echo "  install       build the release and install it to \$$(PREFIX), default /usr/local"
	@echo "  clean         remove the build directory"
	@echo
	@echo "  TEST=<name>   filter the unit tests by name substring"
	@echo "  QUIET=1       print a dot per unit test instead of a line"
	@echo "  INCLUDE=<names>  bundle only these includes, e.g. \"url.bare\""
	@echo "  INCLUDE_EXCLUDE=<names>  bundle all but these includes, e.g. \"qrcode.bare draw.bare\""
	@echo "  libcurl HTTP fetch: $(if $(strip $(CURL_LIBS)),enabled,disabled)"


.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)


#
# The bundled include library source
#
# "src/includeSource.c" and "include/barescript/includeSource.h" are generated and checked in, so a
# fresh clone builds without a bootstrap. Regenerating them runs a BareScript program under a
# command-line interface built from the *existing* generated source - the same self-hosting cycle
# the JavaScript implementation uses to regenerate lib/includeSource.js.
#

COMMA := ,
EMPTY :=
SPACE := $(EMPTY) $(EMPTY)

.PHONY: includes
includes: $(CLI_BIN)
	$(CLI_BIN) $(CURDIR)/bin/includeSource.bare \
	    -v vFiles "'[$(subst $(SPACE),$(COMMA),$(patsubst %,\"$(CURDIR)/%\",$(INCLUDE_LIB_SRCS)))]'" \
	    -v vOutputC "'$(CURDIR)/$(INCLUDE_SOURCE_C)'" \
	    -v vOutputH "'$(CURDIR)/$(INCLUDE_SOURCE_H)'"


#
# Compile
#

.PHONY: compile
compile: $(LIB_SO) $(LIB_A) $(CLI_BIN)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) $(OPT_CFLAGS) $(SO_CFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/test-%.o: $(TEST_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) $(OPT_CFLAGS) -I$(TEST_DIR) -MMD -MP -c -o $@ $<

# The CLI without its entry point, for the test binary, which calls bsMain from a main of its own
$(OBJ_DIR)/bare-test.o: $(SRC_DIR)/bare.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) $(OPT_CFLAGS) -DBARESCRIPT_NO_MAIN -MMD -MP -c -o $@ $<

$(LIB_SO): $(LIB_OBJS)
	$(CC) $(OPT_CFLAGS) $(SO_LDFLAGS) -o $@ $^ $(LIBS)

$(LIB_A): $(LIB_OBJS)
	rm -f $@
	ar rcs $@ $^

$(CLI_BIN): $(CLI_OBJS) $(LIB_SO)
	$(CC) $(OPT_CFLAGS) -o $@ $(CLI_OBJS) -L$(BUILD_DIR) -l$(LIB_NAME) $(RPATH_FLAGS) $(LIBS)


#
# Release - a profile-guided optimization build
#
# PGO is a three-stage build: compile instrumented, run a training workload, then recompile with
# the profile. Each stage compiles one object per source, so the compiles run in parallel under
# "make -j" and only the link-time optimization itself is serial. The training stage below says
# what the workload covers and why.
#

RELEASE_DIR := $(BUILD_DIR)/release
RELEASE_LIB_SO := $(RELEASE_DIR)/lib$(LIB_NAME).$(SO_EXT)
RELEASE_LIB_A := $(RELEASE_DIR)/lib$(LIB_NAME).a
RELEASE_CLI := $(RELEASE_DIR)/$(CLI_NAME)

# -O2, not -O3. On Apple clang the two are equal in speed and -O2 emits 10% less code; on GCC 14
# and aarch64 Linux -O3 is 8.1% slower across the performance suite - every test but one, from
# -0.3% on markdownParse to +23% on mandelbrot - and 2.2% slower on the include test suite.
#
# Not for the reason code size suggests. -O3 does grow .text 30% (173,496 to 225,152 bytes), but
# turning off the passes responsible - vectorization, -fipa-cp-clone, unswitching, peeling - gives
# back a quarter of that growth and none of the time. Nor is it layout: relinking with
# -ffunction-sections and --sort-section=name scatters every hot function, undoing the profile's
# own hot/cold clustering, and costs nothing measurable. The cost is in the code generated for the
# hot functions themselves - mandelbrot, a numeric loop almost entirely inside bsRunCode that
# touches the least other code, regresses the most. All of it measured with the profile applied;
# see PROFILE_MERGE for why that had to be said.
# -flto=auto avoids the lto-wrapper "serial compilation" note on GCC while preserving full LTO.
RELEASE_CFLAGS ?= -O2 -DNDEBUG -flto=auto

# Keep every threaded-dispatch jump distinct. The code generator merges the identical tails of the
# interpreters' handlers - the dispatch to the next instruction - into a few shared indirect
# branches, which the branch-target predictor then cannot tell apart, and each handler reaches
# its shared tail through a jump of its own. Distinct tails are worth 11% of mandelbrot's cycles
# and 15% of nbody's, and an instruction per executed opcode, for 8% more text. Under LTO the
# option has to reach the link-time code generator, so clang takes it on the link line and, for
# the static library's ordinary objects, at compile time; GCC's is a compile option.
ifneq '$(CC_IS_CLANG)' ''
    RELEASE_LDFLAGS := -Wl,-mllvm,-enable-tail-merge=false
    RELEASE_TAIL_CFLAGS := -mllvm -enable-tail-merge=false
else
    RELEASE_LDFLAGS :=
    RELEASE_TAIL_CFLAGS := -fno-crossjumping
    RELEASE_CFLAGS += $(RELEASE_TAIL_CFLAGS)
endif

# The shared library and the command-line interface link from link-time optimization objects. The
# static library keeps the profile but drops link-time optimization, so it stays an archive of
# ordinary object files that any linker consumes rather than one of compiler intermediate code.
RELEASE_LTO_DIR := $(RELEASE_DIR)/lto
RELEASE_LTO_OBJS := $(patsubst $(SRC_DIR)/%.c,$(RELEASE_LTO_DIR)/%.o,$(LIB_SRCS))
RELEASE_OBJ_DIR := $(RELEASE_DIR)/obj
RELEASE_A_OBJS := $(patsubst $(SRC_DIR)/%.c,$(RELEASE_OBJ_DIR)/%.o,$(LIB_SRCS))
RELEASE_A_CFLAGS := $(filter-out -flto -flto=%,$(RELEASE_CFLAGS)) $(RELEASE_TAIL_CFLAGS)

# The instrumented command-line interface and the profile it produces
PGO_DIR := $(BUILD_DIR)/pgo
PGO_CLI := $(PGO_DIR)/$(CLI_NAME)
PGO_OBJS := $(patsubst $(SRC_DIR)/%.c,$(PGO_DIR)/%.o,$(LIB_SRCS) $(CLI_SRCS))
PROFILE_DIR := $(BUILD_DIR)/profile

ifneq '$(CC_IS_CLANG)' ''
    PROFILE_DATA := $(PROFILE_DIR)/barescript.profdata
    PROFILE_GENERATE := -fprofile-generate=$(PROFILE_DIR)
    # The evaluator's indirect calls have many targets (library functions). Clang's default of one
    # value-profile counter per site exhausts the static pool during training: "Unable to track
    # new values: Running out of static counters." Eight is the smallest power of two that covers
    # the training workload.
    PROFILE_GENERATE_CFLAGS := $(PROFILE_GENERATE) -mllvm -vp-counters-per-site=8
    # Clang applies the profile function by function, matching each on a hash of its control
    # flow, and reports a mismatch through -Wbackend-plugin, which -Werror makes fatal: a stale
    # profile fails stage 3 rather than silently not applying. (The -Wprofile-instr-* groups
    # belong to front-end PGO and mean nothing here.) A function with no record at all is not
    # reported, and cannot be asked for: the training executable's LTO drops the 48 public
    # functions the CLI never calls, so -pgo-warn-missing-function would fire on every build, and
    # keeping them with -export_dynamic gives them zero counts, which compiles that API as cold
    # code. So the check on merge below is what would catch a profile that is not this program's.
    PROFILE_USE := -fprofile-use=$(CURDIR)/$(PROFILE_DATA)
    PROFILE_CHECK := bsRunCode bsFunctionCall bsRelease
    PROFILE_MERGE = set -e; \
        xcrun llvm-profdata merge -output=$(PROFILE_DATA) $(PROFILE_DIR)/*.profraw; \
        for fn in $(PROFILE_CHECK); do \
            shown=$$(xcrun llvm-profdata show -function="$$fn" $(PROFILE_DATA) \
                | awk '/^Functions shown:/ { print $$NF }'); \
            [ "$${shown:-0}" -gt 0 ] || \
                { echo "PGO: $(PROFILE_DATA) has no profile for $$fn" >&2; exit 1; }; \
        done
else
    # GCC names each translation unit's profile for the compilation that reads it back: beside the
    # object, "<object>.gcda", or for a source compiled straight to an executable,
    # "<executable>-<unit>.gcda". Training writes them beside the stage-1 objects, and not one of
    # those names is what stage 3 asks for - the shared library's objects, the static library's,
    # and the command-line interface each want a different one. A profile GCC cannot find is not
    # an error, it just silently builds without one, and -Wno-missing-profile hides the warning
    # that would say so. So copy each trained profile to every name stage 3 looks for.
    PROFILE_DATA := $(PROFILE_DIR)/gcda.stamp
    PROFILE_GENERATE := -fprofile-generate
    PROFILE_GENERATE_CFLAGS := $(PROFILE_GENERATE) -fprofile-update=single
    PROFILE_USE := -fprofile-use -fprofile-correction -Wno-missing-profile -Wno-clobbered
    PROFILE_MERGE = set -e; mkdir -p $(RELEASE_LTO_DIR) $(RELEASE_OBJ_DIR); \
        for tu in $(notdir $(basename $(LIB_SRCS))); do \
            cp -f $(PGO_DIR)/$$tu.gcda $(RELEASE_LTO_DIR)/$$tu.gcda; \
            cp -f $(PGO_DIR)/$$tu.gcda $(RELEASE_OBJ_DIR)/$$tu.gcda; \
        done; \
        for tu in $(notdir $(basename $(CLI_SRCS))); do \
            cp -f $(PGO_DIR)/$$tu.gcda $(RELEASE_DIR)/$(CLI_NAME)-$$tu.gcda; \
        done; \
        touch $(PROFILE_DATA)
endif

.PHONY: release
release: $(RELEASE_CLI) $(RELEASE_LIB_A)
	@echo
	@echo "Release build: $(RELEASE_CLI)"

# Stage 1 - build instrumented
$(PGO_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) $(RELEASE_CFLAGS) $(PROFILE_GENERATE_CFLAGS) -MMD -MP -c -o $@ $<

$(PGO_CLI): $(PGO_OBJS)
	$(CC) $(RELEASE_CFLAGS) $(PROFILE_GENERATE) -o $@ $^ $(LIBS)

# Stage 2 - run the training workload
#
# Two programs, merged by count: the performance suite, which is the benchmark itself, and the
# include library test suite, which parses about 2 MB of BareScript from source and runs every
# include library function - the path "bare script.bare" takes, and one the performance suite
# never does since it loads bundled models. A synthetic parse script, the language tests, and a
# static-analysis run were measured and moved neither workload beyond build-to-build noise. The
# two are independent, so they run at once; %p so each process writes its own profraw, then merge.
# A merge whose check fails must not leave its output behind: make would take it as up to date,
# and the next run would go straight to stage 3, past the check.
PROFILE_ENV := LLVM_PROFILE_FILE="$(CURDIR)/$(PROFILE_DIR)/default_%p.profraw"
$(PROFILE_DATA): $(PGO_CLI) $(PERF_DIR)/test.bare $(INCLUDE_LIB_SRCS) $(INCLUDE_TEST_SRCS)
	@rm -rf $(PROFILE_DIR) $(PGO_DIR)/*.gcda
	@mkdir -p $(PROFILE_DIR)
	$(PROFILE_ENV) $(PGO_CLI) $(PERF_DIR)/test.bare > /dev/null & \
	$(PROFILE_ENV) $(PGO_CLI) -d -m $(INCLUDE_TEST_DIR)/runTests.bare > /dev/null; status=$$?; \
	wait $$! && exit $$status
	( $(PROFILE_MERGE) ) || { rm -rf $(PROFILE_DATA); exit 1; }

# Stage 3 - rebuild with the profile
$(RELEASE_LTO_DIR)/%.o: $(SRC_DIR)/%.c $(PROFILE_DATA)
	@mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) $(RELEASE_CFLAGS) $(PROFILE_USE) $(SO_CFLAGS) -c -o $@ $<

$(RELEASE_OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(PROFILE_DATA)
	@mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) $(RELEASE_A_CFLAGS) $(PROFILE_USE) $(SO_CFLAGS) -c -o $@ $<

$(RELEASE_LIB_SO): $(RELEASE_LTO_OBJS)
	$(CC) $(RELEASE_CFLAGS) $(PROFILE_USE) $(RELEASE_LDFLAGS) $(SO_LDFLAGS) -o $@ $^ $(LIBS)

$(RELEASE_CLI): $(RELEASE_LIB_SO)
	$(CC) $(BASE_CFLAGS) $(RELEASE_CFLAGS) $(PROFILE_USE) $(RELEASE_LDFLAGS) -o $@ $(CLI_SRCS) \
	    -L$(RELEASE_DIR) -l$(LIB_NAME) $(RPATH_FLAGS) $(LIBS)

$(RELEASE_LIB_A): $(RELEASE_A_OBJS)
	rm -f $@
	ar rcs $@ $^


#
# Test
#

$(TEST_BIN): $(TEST_OBJS) $(OBJ_DIR)/bare-test.o $(LIB_A)
	$(CC) -o $@ $^ $(LIBS) $(TEST_LIBS)

.PHONY: test
test: $(TEST_BIN)
	$(TEST_BIN) $(if $(QUIET),-q )$(TEST)


#
# Coverage
#

# The thread test runs eight runtimes at once, and gcov's counters are a plain read-modify-write
# by default - concurrent updates lose increments, and gcov then solves the flow graph into
# negative counts that report covered lines as missed. Atomic counters make the run repeatable.
COVER_CFLAGS := --coverage -O0 -g $(call CC_SUPPORTS,-fprofile-update=atomic)

$(COVER_DIR)/bare.o: COVER_CFLAGS += -DBARESCRIPT_NO_MAIN
$(COVER_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) $(COVER_CFLAGS) -MMD -MP -c -o $@ $<

$(COVER_BIN): $(COVER_OBJS)
	$(CC) --coverage -o $@ $^ $(LIBS) $(TEST_LIBS)

# gcov names each report for its source and writes it to the current directory, which must be the
# one the objects were compiled in for it to find the source - so the reports are moved afterwards
GCOV := $(if $(CC_IS_CLANG),xcrun llvm-cov gcov,gcov)
COVER_GCOV := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/coverage/%.c.gcov,$(COVER_SRCS))

.PHONY: cover
cover: $(COVER_BIN)
	rm -f $(COVER_DIR)/*.gcda $(BUILD_DIR)/coverage/*.gcov
	$(COVER_BIN) $(if $(QUIET),-q )$(TEST)
	@mkdir -p $(BUILD_DIR)/coverage
	$(GCOV) -o $(COVER_DIR) $(COVER_SRCS) > /dev/null
	@mv *.gcov $(BUILD_DIR)/coverage/
	@awk -v verbose=$(if $(VERBOSE),1,0) -f $(TEST_DIR)/coverage.awk $(COVER_GCOV)


#
# The BareScript include library test suite and the language test suite
#
# The same three include library runs the JavaScript and Python implementations make, over the
# same test scripts, so the reports are directly comparable. The language tests are this
# project's own suite. Each suite is its own target, so under "make -j" they run at once.
#

.PHONY: test-include test-include-lint test-include-markdownup test-include-run test-language
test-include: test-include-lint test-include-markdownup test-include-run
test-include-lint test-include-markdownup test-include-run test-language: BARE := $(CLI_BIN)
test-include-lint test-include-markdownup test-include-run test-language: $(CLI_BIN)

# The release build under the same suites. The shipped binary is built by a different pipeline -
# profile-guided, link-time optimized - than the one every other target tests; running the suites
# against it is what catches a miscompile, or a profile that silently failed to apply. Each suite
# is its own target, mirroring the development-build targets, so one suite can be run alone
# against the release binary - "make test-release-run" is the suite's timed report.
.PHONY: test-release test-release-lint test-release-markdownup test-release-run test-release-language
test-release: test-release-lint test-release-markdownup test-release-run test-release-language
test-release-lint test-release-markdownup test-release-run test-release-language: BARE := $(RELEASE_CLI)
test-release-lint test-release-markdownup test-release-run test-release-language: $(RELEASE_CLI)

test-include-lint test-release-lint:
	$(BARE) -x -m $(INCLUDE_LIB_SRCS) $(sort $(wildcard $(INCLUDE_TEST_DIR)/test*.bare))
	$(BARE) -s -m $(INCLUDE_TEST_DIR)/runTests.bare $(INCLUDE_TEST_DIR)/runTestsMarkdownUp.bare

test-include-markdownup test-release-markdownup:
	$(BARE) -d -v vUnittestReport true \
	    $(INCLUDE_TEST_DIR)/runTestsMarkdownUp.bare$(if $(TEST), -v vUnittestTest "'$(TEST)'")

test-include-run test-release-run:
	$(BARE) -d -m $(INCLUDE_TEST_DIR)/runTests.bare$(if $(TEST), -v vUnittestTest "'$(TEST)'")

test-language test-release-language:
	$(BARE) -d -m $(TEST_DIR)/include/runTests.bare


#
# Performance
#
# The same suite the JavaScript and Python implementations run, reported the same way, so the
# results are directly comparable. Timings are written to a temporary CSV and moved into place on
# success, so build/perf.csv is always a complete, valid CSV.
#
# This measures the release build, not the development one. The implementations it is compared
# against are themselves optimized builds - Node ships as one, and CPython is built with profile-
# guided and link-time optimization - so timing the "-O2 -g" build here would understate this
# runtime by about 1.2x against them. The native baseline gets the same optimization level.
#

PERF_BARE_JS_DIR := ../bare-script
PERF_BARE_PY_DIR := ../bare-script-py
PERF_CSV := $(BUILD_DIR)/perf.csv
PERF_CSV_TMP := $(BUILD_DIR)/perf-$$PPID.csv
PERF_MERGE := 1
PERF_REPORT := 1
PERF_RUNS := 2
PERF_NATIVE := $(BUILD_DIR)/perf-native

.PHONY: perf
perf: $(RELEASE_CLI) $(PERF_NATIVE)
	echo "language,test,runs,timeMs" > $(PERF_CSV_TMP)
	set -e; for X in $$(seq 1 $(PERF_RUNS)); do \
	    echo "Run $$X of $(PERF_RUNS) - BareScript (C)"; \
	    $(RELEASE_CLI) $(PERF_DIR)/test.bare -v vLanguage "'BareScript (C)'"$(if $(TEST), -v vTest "'$(TEST)'") \
	        >> $(PERF_CSV_TMP); \
	    echo "Run $$X of $(PERF_RUNS) - C"; \
	    $(PERF_NATIVE) "C"$(if $(TEST), "$(TEST)") >> $(PERF_CSV_TMP); \
	done
ifneq '$(PERF_MERGE)' ''
ifneq '$(wildcard $(PERF_BARE_JS_DIR))' ''
	$(MAKE) -C $(PERF_BARE_JS_DIR) perf PERF_RUNS=$(PERF_RUNS) TEST=$(TEST) PERF_MERGE= PERF_REPORT=
	tail -n +2 $(PERF_BARE_JS_DIR)/$(PERF_CSV) >> $(PERF_CSV_TMP)
endif
ifneq '$(wildcard $(PERF_BARE_PY_DIR))' ''
	$(MAKE) -C $(PERF_BARE_PY_DIR) perf PERF_RUNS=$(PERF_RUNS) TEST=$(TEST) PERF_MERGE= PERF_REPORT=
	tail -n +2 $(PERF_BARE_PY_DIR)/$(PERF_CSV) >> $(PERF_CSV_TMP)
endif
endif
	mv $(PERF_CSV_TMP) $(PERF_CSV)
ifneq '$(PERF_REPORT)' ''
	$(RELEASE_CLI) $(CURDIR)/bin/perfReport.bare -v vCSV "'$(CURDIR)/$(PERF_CSV)'"
endif

# The native C baseline, for the tests it implements
$(PERF_NATIVE): $(PERF_DIR)/test.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) $(RELEASE_CFLAGS) -o $@ $< -lm


#
# The cross-language performance suite - real-world-like applications ported to BareScript,
# JavaScript, Python, Lua, Ruby, and Perl, measured under the release build. See perfx/README.md.
# PERFX_ARGS passes options through, e.g. make perfx PERFX_ARGS="--apps nbody --runs 5".
#

.PHONY: perfx perfx-check
perfx: $(RELEASE_CLI)
	python3 perfx/perfx.py --bare $(RELEASE_CLI) --out $(BUILD_DIR)/perfx $(PERFX_ARGS)

# Run every port at a small workload and check that they all compute the same result
perfx-check: $(RELEASE_CLI)
	python3 perfx/perfx.py --bare $(RELEASE_CLI) --check


#
# Install
#

PREFIX ?= /usr/local

# Install the release build - the development build is a third slower and carries debug symbols
.PHONY: install
install: release
	install -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/include/barescript
	install -m 755 $(RELEASE_LIB_SO) $(DESTDIR)$(PREFIX)/lib/
	install -m 644 $(RELEASE_LIB_A) $(DESTDIR)$(PREFIX)/lib/
	install -m 755 $(RELEASE_CLI) $(DESTDIR)$(PREFIX)/bin/
	install -m 644 $(INC_DIR)/barescript/*.h $(DESTDIR)$(PREFIX)/include/barescript/


-include $(wildcard $(OBJ_DIR)/*.d $(COVER_DIR)/*.d $(PGO_DIR)/*.d)
