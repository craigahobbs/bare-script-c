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
    SO_LDFLAGS = -dynamiclib -install_name @rpath/lib$(LIB_NAME).$(SO_EXT)
    RPATH_FLAGS := -Wl,-rpath,@executable_path -Wl,-rpath,@executable_path/../lib
else
    SO_EXT := so
    SO_LDFLAGS = -shared -Wl,-soname,lib$(LIB_NAME).$(SO_EXT)
    RPATH_FLAGS := -Wl,-rpath,'$$ORIGIN' -Wl,-rpath,'$$ORIGIN/../lib'
endif


# Toolchain
CC ?= cc
CC_IS_CLANG := $(shell $(CC) --version 2>/dev/null | grep -c -i clang)
WARN_FLAGS := -Wall -Wextra -Werror -Wno-unused-parameter -Wshadow -Wpointer-arith \
    -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes -Wwrite-strings \
    -Wno-clobbered
BASE_CFLAGS := -std=c11 -pedantic -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -I$(INC_DIR) $(WARN_FLAGS)

# No caller reads errno after a libm call. Without this, GCC and clang on Linux wrap every sqrt,
# floor, and fmod in an errno check that keeps them out of line; Apple's clang already assumes it,
# so this makes the ELF build match the code generation that was measured.
BASE_CFLAGS += -fno-math-errno
OPT_CFLAGS ?= -O2 -g
LIBS := -lm

# Use a flag only where the toolchain accepts it - the argument must not contain a comma
CC_SUPPORTS = $(shell echo 'int main(void){return 0;}' | \
    $(CC) -Werror $(1) -x c - -o /dev/null > /dev/null 2>&1 && echo $(1))

# Shared library code generation
#
# Without this, a call from one translation unit of the library to another goes through the PLT on
# ELF targets, because the symbol could be interposed at load time - which also blocks inlining
# across the library. Nothing here is meant to be interposed. Mach-O binds these calls directly
# already, and Apple's clang rejects the flag, so the probe leaves it out there.
SO_CFLAGS := -fPIC -fvisibility=hidden $(call CC_SUPPORTS,-fno-semantic-interposition)


# Optional libcurl support for the HTTP fetch function
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null || curl-config --cflags 2>/dev/null)
CURL_LIBS := $(shell pkg-config --libs libcurl 2>/dev/null || curl-config --libs 2>/dev/null)
ifneq '$(strip $(CURL_LIBS))' ''
    BASE_CFLAGS += -DBARESCRIPT_CURL $(CURL_CFLAGS)
    LIBS += $(CURL_LIBS)
endif


# The bundled BareScript include library
INCLUDE_LIB_DIR := lib/include
INCLUDE_LIB_SRCS := $(sort $(wildcard $(INCLUDE_LIB_DIR)/*.bare))
INCLUDE_TEST_DIR := $(INCLUDE_LIB_DIR)/test
INCLUDE_SOURCE_C := $(SRC_DIR)/includeSource.c
INCLUDE_SOURCE_H := $(INC_DIR)/barescript/includeSource.h


# Sources
LIB_SRCS := $(sort $(wildcard $(SRC_DIR)/*.c))
LIB_SRCS := $(filter-out $(SRC_DIR)/main.c $(SRC_DIR)/bare.c,$(LIB_SRCS))
CLI_SRCS := $(SRC_DIR)/bare.c $(SRC_DIR)/main.c
TEST_SRCS := $(sort $(wildcard $(TEST_DIR)/*.c))


# Object files
OBJ_DIR := $(BUILD_DIR)/obj
COVER_DIR := $(BUILD_DIR)/cover
LIB_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(LIB_SRCS))
CLI_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(CLI_SRCS))
COVER_SRCS := $(LIB_SRCS) $(SRC_DIR)/bare.c
COVER_OBJS := $(patsubst $(SRC_DIR)/%.c,$(COVER_DIR)/%.o,$(COVER_SRCS)) \
    $(patsubst $(TEST_DIR)/%.c,$(COVER_DIR)/test-%.o,$(TEST_SRCS))
TEST_OBJS := $(patsubst $(TEST_DIR)/%.c,$(OBJ_DIR)/test-%.o,$(TEST_SRCS))


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
# static analysis run - and this project's own language tests.
#

.PHONY: commit
commit: test cover test-include test-language


.PHONY: help
help:
	@echo "usage: make [commit|compile|test|cover|test-include|test-language|perf|release|includes|install|clean]"
	@echo
	@echo "  commit        everything that must pass before a commit"
	@echo "  compile       build the shared library and the command-line interface"
	@echo "  test          build and run the unit tests"
	@echo "  cover         run the unit tests and report coverage (fails under 100%)"
	@echo "                VERBOSE=1 lists each uncovered line"
	@echo "  test-include  run the BareScript include library test suite"
	@echo "  test-language run this project's own BareScript language tests"
	@echo "  perf          run the performance suite against the release build"
	@echo "  release       profile-guided optimization build in build/release"
	@echo "  includes      regenerate the bundled include library source"
	@echo "  install       build the release and install it to \$$(PREFIX), default /usr/local"
	@echo "  clean         remove the build directory"
	@echo
	@echo "  TEST=<name>   filter the unit tests by name substring"
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
	BARESCRIPT_INCLUDE_PATH=$(CURDIR)/$(INCLUDE_LIB_DIR) $(CLI_BIN) $(CURDIR)/bin/includeSource.bare \
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

$(LIB_SO): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OPT_CFLAGS) $(SO_LDFLAGS) -o $@ $^ $(LIBS)

$(LIB_A): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	ar rcs $@ $^

$(CLI_BIN): $(CLI_OBJS) $(LIB_SO)
	@mkdir -p $(dir $@)
	$(CC) $(OPT_CFLAGS) -o $@ $(CLI_OBJS) -L$(BUILD_DIR) -l$(LIB_NAME) $(RPATH_FLAGS) $(LIBS)


#
# Release - a profile-guided optimization build
#
# PGO is a three-stage build: compile instrumented, run a training workload, then recompile with
# the profile. The training stage below says what the workload covers and why.
#

RELEASE_DIR := $(BUILD_DIR)/release
RELEASE_OBJ_DIR := $(RELEASE_DIR)/obj
PROFILE_DIR := $(BUILD_DIR)/profile
# -O2 and -O3 are equal in speed here at every stage; under PGO and LTO, -O2 emits 10% less code.
# -flto=auto avoids the lto-wrapper "serial compilation" note on GCC while preserving full LTO.
RELEASE_CFLAGS ?= -O2 -DNDEBUG -flto=auto
RELEASE_LIB_SO := $(RELEASE_DIR)/lib$(LIB_NAME).$(SO_EXT)
RELEASE_LIB_A := $(RELEASE_DIR)/lib$(LIB_NAME).a
RELEASE_CLI := $(RELEASE_DIR)/$(CLI_NAME)
RELEASE_A_OBJS := $(patsubst $(SRC_DIR)/%.c,$(RELEASE_OBJ_DIR)/%.o,$(LIB_SRCS))

# The static library keeps the profile but drops link-time optimization, so it stays an archive of
# ordinary object files that any linker consumes rather than one of compiler intermediate code
RELEASE_A_CFLAGS := $(filter-out -flto -flto=%,$(RELEASE_CFLAGS))

ifneq '$(filter-out 0,$(CC_IS_CLANG))' ''
    PROFILE_DATA := $(PROFILE_DIR)/barescript.profdata
    # The evaluator's indirect calls have many targets (library functions). Clang's
    # default of one value-profile counter per site exhausts the static pool during
    # training: "Unable to track new values: Running out of static counters."
    # Eight is the smallest power of two that covers the training workload.
    PROFILE_GENERATE := -fprofile-generate=$(PROFILE_DIR) -mllvm -vp-counters-per-site=8
    PROFILE_USE = -fprofile-use=$(CURDIR)/$(PROFILE_DATA) -Wno-profile-instr-unprofiled \
        -Wno-profile-instr-out-of-date
    PROFILE_MERGE = xcrun llvm-profdata merge -output=$(PROFILE_DATA) $(PROFILE_DIR)/*.profraw
else
    PROFILE_DATA := $(PROFILE_DIR)
    PROFILE_GENERATE := -fprofile-generate=$(PROFILE_DIR) -fprofile-update=single
    PROFILE_USE = -fprofile-use=$(PROFILE_DIR) -fprofile-correction -Wno-missing-profile -Wno-clobbered
    PROFILE_MERGE = :
endif

.PHONY: release
release: $(RELEASE_CLI) $(RELEASE_LIB_A)
	@echo
	@echo "Release build: $(RELEASE_CLI)"

# Stage 1 and 2 - build instrumented and run the training workload
#
# Two programs, merged by count: the performance suite, which is the benchmark itself, and the
# include library test suite, which parses about 2 MB of BareScript from source and runs every
# include library function - the path "bare script.bare" takes, and one the performance suite
# never does since it loads bundled models. A synthetic parse script, the language tests, and a
# static-analysis run were measured and moved neither workload beyond build-to-build noise. %p so
# each process writes its own profraw, then merge.
$(PROFILE_DATA): $(LIB_SRCS) $(CLI_SRCS) $(PERF_DIR)/test.bare $(INCLUDE_LIB_SRCS) \
        $(sort $(wildcard $(INCLUDE_TEST_DIR)/*.bare))
	@rm -rf $(PROFILE_DIR) $(BUILD_DIR)/pgo
	@mkdir -p $(PROFILE_DIR) $(BUILD_DIR)/pgo
	$(CC) $(BASE_CFLAGS) $(RELEASE_CFLAGS) $(PROFILE_GENERATE) -o $(BUILD_DIR)/pgo/$(CLI_NAME) \
	    $(LIB_SRCS) $(CLI_SRCS) $(LIBS)
	LLVM_PROFILE_FILE="$(CURDIR)/$(PROFILE_DIR)/default_%p.profraw" \
	    $(BUILD_DIR)/pgo/$(CLI_NAME) $(PERF_DIR)/test.bare > /dev/null
	LLVM_PROFILE_FILE="$(CURDIR)/$(PROFILE_DIR)/default_%p.profraw" \
	    $(BUILD_DIR)/pgo/$(CLI_NAME) -d -m $(INCLUDE_TEST_DIR)/runTests.bare > /dev/null
	$(PROFILE_MERGE)

# Stage 3 - rebuild with the profile
$(RELEASE_LIB_SO): $(PROFILE_DATA)
	@mkdir -p $(RELEASE_DIR)
	$(CC) $(BASE_CFLAGS) $(RELEASE_CFLAGS) $(PROFILE_USE) $(SO_CFLAGS) $(SO_LDFLAGS) \
	    -o $@ $(LIB_SRCS) $(LIBS)

$(RELEASE_CLI): $(RELEASE_LIB_SO)
	@mkdir -p $(RELEASE_DIR)
	$(CC) $(BASE_CFLAGS) $(RELEASE_CFLAGS) $(PROFILE_USE) -o $@ $(CLI_SRCS) \
	    -L$(RELEASE_DIR) -l$(LIB_NAME) $(RPATH_FLAGS) $(LIBS)

$(RELEASE_OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(PROFILE_DATA)
	@mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) $(RELEASE_A_CFLAGS) $(PROFILE_USE) $(SO_CFLAGS) -c -o $@ $<

$(RELEASE_LIB_A): $(RELEASE_A_OBJS)
	rm -f $@
	ar rcs $@ $^


#
# Test
#

$(TEST_BIN): $(TEST_OBJS) $(OBJ_DIR)/bare.o $(LIB_A)
	@mkdir -p $(dir $@)
	$(CC) -o $@ $(TEST_OBJS) $(OBJ_DIR)/bare.o $(LIB_A) $(LIBS)

.PHONY: test
test: $(TEST_BIN)
	$(TEST_BIN) $(TEST)


#
# Coverage
#

COVER_CFLAGS := --coverage -O0 -g

$(COVER_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) $(COVER_CFLAGS) -MMD -MP -c -o $@ $<

$(COVER_DIR)/test-%.o: $(TEST_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) $(COVER_CFLAGS) -I$(TEST_DIR) -MMD -MP -c -o $@ $<

$(COVER_BIN): $(COVER_OBJS)
	@mkdir -p $(dir $@)
	$(CC) --coverage -o $@ $^ $(LIBS)

GCOV := $(if $(filter-out 0,$(CC_IS_CLANG)),xcrun llvm-cov gcov,gcov)
COVER_GCOV := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/coverage/%.c.gcov,$(COVER_SRCS))

.PHONY: cover
cover: $(COVER_BIN)
	rm -f $(COVER_DIR)/*.gcda $(BUILD_DIR)/coverage/*.gcov
	$(COVER_BIN) $(TEST)
	@mkdir -p $(BUILD_DIR)/coverage
	$(GCOV) -o $(COVER_DIR) $(COVER_SRCS) > /dev/null
	@mv *.gcov $(BUILD_DIR)/coverage/
	@awk -v verbose=$(if $(VERBOSE),1,0) -f $(TEST_DIR)/coverage.awk $(COVER_GCOV)


#
# The BareScript include library test suite
#
# The same three runs the JavaScript and Python implementations make, over the same test scripts,
# so the reports are directly comparable.
#

.PHONY: test-include test-include-lint test-include-markdownup test-include-run
test-include: test-include-lint test-include-markdownup test-include-run
test-include-lint test-include-markdownup test-include-run: compile

test-include-lint:
	$(CLI_BIN) -x -m $(INCLUDE_LIB_SRCS) $(sort $(wildcard $(INCLUDE_TEST_DIR)/test*.bare))
	$(CLI_BIN) -s -m $(INCLUDE_TEST_DIR)/runTests.bare $(INCLUDE_TEST_DIR)/runTestsMarkdownUp.bare

test-include-markdownup:
	$(CLI_BIN) -d -v vUnittestReport true \
	    $(INCLUDE_TEST_DIR)/runTestsMarkdownUp.bare$(if $(TEST), -v vUnittestTest "'$(TEST)'")

test-include-run:
	$(CLI_BIN) -d -m $(INCLUDE_TEST_DIR)/runTests.bare$(if $(TEST), -v vUnittestTest "'$(TEST)'")


#
# The BareScript language test suite
#

.PHONY: test-language
test-language: compile
	$(CLI_BIN) $(if $(DEBUG),-d )$(TEST_DIR)/include/runTests.bare


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
	mkdir -p $(dir $(PERF_CSV_TMP))
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


-include $(wildcard $(OBJ_DIR)/*.d) $(wildcard $(COVER_DIR)/*.d)
