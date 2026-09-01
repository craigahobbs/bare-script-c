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
    RPATH_FLAGS := -Wl,-rpath,@executable_path
else
    SO_EXT := so
    SO_LDFLAGS = -shared -Wl,-soname,lib$(LIB_NAME).$(SO_EXT)
    RPATH_FLAGS := -Wl,-rpath,'$$ORIGIN'
endif


# Toolchain
CC ?= cc
CC_IS_CLANG := $(shell $(CC) --version 2>/dev/null | grep -c -i clang)
WARN_FLAGS := -Wall -Wextra -Werror -Wno-unused-parameter -Wshadow -Wpointer-arith \
    -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes -Wwrite-strings
BASE_CFLAGS := -std=c11 -pedantic -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -I$(INC_DIR) $(WARN_FLAGS)
OPT_CFLAGS ?= -O2 -g
LIBS := -lm


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
INCLUDE_SOURCE_C := $(SRC_DIR)/includeSource.c
INCLUDE_SOURCE_H := $(INC_DIR)/barescript/includeSource.h


# Sources
LIB_SRCS := $(sort $(wildcard $(SRC_DIR)/*.c))
LIB_SRCS := $(filter-out $(SRC_DIR)/main.c,$(LIB_SRCS))
TEST_SRCS := $(sort $(wildcard $(TEST_DIR)/*.c))


# Object files
OBJ_DIR := $(BUILD_DIR)/obj
COVER_DIR := $(BUILD_DIR)/cover
LIB_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(LIB_SRCS))
COVER_OBJS := $(patsubst $(SRC_DIR)/%.c,$(COVER_DIR)/%.o,$(LIB_SRCS)) \
    $(patsubst $(TEST_DIR)/%.c,$(COVER_DIR)/test-%.o,$(TEST_SRCS))
TEST_OBJS := $(patsubst $(TEST_DIR)/%.c,$(OBJ_DIR)/test-%.o,$(TEST_SRCS))


# Build outputs
LIB_SO := $(BUILD_DIR)/lib$(LIB_NAME).$(SO_EXT)
LIB_A := $(BUILD_DIR)/lib$(LIB_NAME).a
CLI_BIN := $(BUILD_DIR)/$(CLI_NAME)
TEST_BIN := $(BUILD_DIR)/$(CLI_NAME)-test
COVER_BIN := $(BUILD_DIR)/$(CLI_NAME)-cover
COVER_CLI := $(BUILD_DIR)/$(CLI_NAME)-cover-cli


.PHONY: help
help:
	@echo "usage: make [compile|test|cover|test-include|perf|release|install|clean]"
	@echo
	@echo "  compile       build the shared library and the command-line interface"
	@echo "  test          build and run the unit tests"
	@echo "  cover         run the unit tests and report coverage (fails under 100%)"
	@echo "                VERBOSE=1 lists each uncovered line"
	@echo "  test-include  run the BareScript language test suite with the built CLI"
	@echo "  perf          run the performance suite"
	@echo "  release       profile-guided optimization build in build/release"
	@echo "  includes      regenerate the bundled include library source"
	@echo "  install       install to \$$(PREFIX), default /usr/local"
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

.PHONY: includes
includes:
	$(MAKE) $(CLI_BIN)
	$(CLI_BIN) $(CURDIR)/bin/includeSource.bare \
	    -v vFiles "'[$(subst $(SPACE),$(COMMA),$(patsubst %,\"$(CURDIR)/%\",$(INCLUDE_LIB_SRCS)))]'" \
	    -v vOutputC "'$(CURDIR)/$(INCLUDE_SOURCE_C)'" \
	    -v vOutputH "'$(CURDIR)/$(INCLUDE_SOURCE_H)'"

COMMA := ,
EMPTY :=
SPACE := $(EMPTY) $(EMPTY)

.PHONY: superclean
superclean: clean


#
# Compile
#

.PHONY: compile
compile: $(LIB_SO) $(LIB_A) $(CLI_BIN)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) $(OPT_CFLAGS) -fPIC -MMD -MP -c -o $@ $<

$(OBJ_DIR)/test-%.o: $(TEST_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) $(OPT_CFLAGS) -I$(TEST_DIR) -MMD -MP -c -o $@ $<

$(LIB_SO): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(SO_LDFLAGS) -o $@ $^ $(LIBS)

$(LIB_A): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	ar rcs $@ $^

$(CLI_BIN): $(OBJ_DIR)/main.o $(LIB_SO)
	@mkdir -p $(dir $@)
	$(CC) -o $@ $(OBJ_DIR)/main.o -L$(BUILD_DIR) -l$(LIB_NAME) $(RPATH_FLAGS) $(LIBS)


#
# Release - a profile-guided optimization build
#
# PGO is a three-stage build: compile instrumented, run a training workload, then recompile with
# the profile. See perf/train.bare for what the training workload covers and why.
#

RELEASE_DIR := $(BUILD_DIR)/release
PROFILE_DIR := $(BUILD_DIR)/profile
RELEASE_CFLAGS ?= -O3 -DNDEBUG -flto
RELEASE_LIB_SO := $(RELEASE_DIR)/lib$(LIB_NAME).$(SO_EXT)
RELEASE_LIB_A := $(RELEASE_DIR)/lib$(LIB_NAME).a
RELEASE_CLI := $(RELEASE_DIR)/$(CLI_NAME)

ifneq '$(filter-out 0,$(CC_IS_CLANG))' ''
    PROFILE_DATA := $(PROFILE_DIR)/barescript.profdata
    PROFILE_GENERATE := -fprofile-generate=$(PROFILE_DIR)
    PROFILE_USE = -fprofile-use=$(CURDIR)/$(PROFILE_DATA) -Wno-profile-instr-unprofiled \
        -Wno-profile-instr-out-of-date
    PROFILE_MERGE = xcrun llvm-profdata merge -output=$(PROFILE_DATA) $(PROFILE_DIR)/*.profraw
else
    PROFILE_DATA := $(PROFILE_DIR)
    PROFILE_GENERATE := -fprofile-generate=$(PROFILE_DIR) -fprofile-update=single
    PROFILE_USE = -fprofile-use=$(PROFILE_DIR) -fprofile-correction -Wno-missing-profile
    PROFILE_MERGE = :
endif

.PHONY: release
release: $(RELEASE_CLI)
	@echo
	@echo "Release build: $(RELEASE_CLI)"

# Stage 1 and 2 - build instrumented and run the training workload
$(PROFILE_DATA): $(LIB_SRCS) $(SRC_DIR)/main.c $(PERF_DIR)/train.bare $(PERF_DIR)/test.bare
	@rm -rf $(PROFILE_DIR) $(BUILD_DIR)/pgo
	@mkdir -p $(PROFILE_DIR) $(BUILD_DIR)/pgo
	$(CC) $(BASE_CFLAGS) $(RELEASE_CFLAGS) $(PROFILE_GENERATE) -o $(BUILD_DIR)/pgo/$(CLI_NAME) \
	    $(LIB_SRCS) $(SRC_DIR)/main.c $(LIBS)
	$(BUILD_DIR)/pgo/$(CLI_NAME) $(CURDIR)/$(PERF_DIR)/train.bare \
	    -v vIncludeDir "'$(CURDIR)/$(INCLUDE_LIB_DIR)'"
	$(BUILD_DIR)/pgo/$(CLI_NAME) $(PERF_DIR)/test.bare > /dev/null
	$(BUILD_DIR)/pgo/$(CLI_NAME) $(TEST_DIR)/include/runTests.bare > /dev/null
	$(BUILD_DIR)/pgo/$(CLI_NAME) -s $(TEST_DIR)/include/testLibrary.bare > /dev/null
	$(PROFILE_MERGE)

# Stage 3 - rebuild with the profile
$(RELEASE_CLI): $(PROFILE_DATA)
	@mkdir -p $(RELEASE_DIR)
	$(CC) $(BASE_CFLAGS) $(RELEASE_CFLAGS) $(PROFILE_USE) -fPIC $(SO_LDFLAGS) \
	    -o $(RELEASE_LIB_SO) $(LIB_SRCS) $(LIBS)
	$(CC) $(BASE_CFLAGS) $(RELEASE_CFLAGS) $(PROFILE_USE) -o $@ $(SRC_DIR)/main.c \
	    -L$(RELEASE_DIR) -l$(LIB_NAME) $(RPATH_FLAGS) $(LIBS)


#
# Test
#

$(TEST_BIN): $(TEST_OBJS) $(LIB_A)
	@mkdir -p $(dir $@)
	$(CC) -o $@ $(TEST_OBJS) $(LIB_A) $(LIBS)

.PHONY: test
test: $(TEST_BIN)
	$(TEST_BIN) $(TEST)


#
# Coverage
#

COVER_CFLAGS := --coverage -O0 -g -DBARESCRIPT_COVERAGE

$(COVER_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) $(COVER_CFLAGS) -MMD -MP -c -o $@ $<

$(COVER_DIR)/test-%.o: $(TEST_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) $(COVER_CFLAGS) -I$(TEST_DIR) -MMD -MP -c -o $@ $<

$(COVER_BIN): $(COVER_OBJS)
	@mkdir -p $(dir $@)
	$(CC) --coverage -o $@ $^ $(LIBS)

.PHONY: cover
cover: $(COVER_BIN)
	rm -f $(COVER_DIR)/*.gcda $(BUILD_DIR)/coverage/*.gcov
	$(COVER_BIN) $(TEST)
	@mkdir -p $(BUILD_DIR)/coverage
	@rm -f $(BUILD_DIR)/coverage/*.gcov
	$(GCOV) -o $(COVER_DIR) $(LIB_SRCS) > /dev/null
	@mv *.gcov $(BUILD_DIR)/coverage/
	@$(AWK) -v verbose=$(if $(VERBOSE),1,0) -f $(TEST_DIR)/coverage.awk $(COVER_GCOV)

GCOV := $(if $(filter-out 0,$(CC_IS_CLANG)),xcrun llvm-cov gcov,gcov)
AWK := awk
COVER_GCOV := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/coverage/%.c.gcov,$(LIB_SRCS))


#
# BareScript language test suite
#

.PHONY: test-include
test-include: compile
	$(CLI_BIN) $(if $(DEBUG),-d )$(TEST_DIR)/include/runTests.bare


#
# Performance
#

PERF_RUNS ?= 3

.PHONY: perf
perf: compile
	@echo "language,test,runs,timeMs"
	@set -e; for X in $$(seq 1 $(PERF_RUNS)); do \
	    $(CLI_BIN) $(PERF_DIR)/test.bare -v vLanguage "'BareScript (C)'"$(if $(TEST), -v vTest "'$(TEST)'"); \
	done


#
# Install
#

PREFIX ?= /usr/local

.PHONY: install
install: compile
	install -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/include/barescript
	install -m 755 $(LIB_SO) $(DESTDIR)$(PREFIX)/lib/
	install -m 644 $(LIB_A) $(DESTDIR)$(PREFIX)/lib/
	install -m 755 $(CLI_BIN) $(DESTDIR)$(PREFIX)/bin/
	install -m 644 $(INC_DIR)/barescript/*.h $(DESTDIR)$(PREFIX)/include/barescript/


-include $(wildcard $(OBJ_DIR)/*.d) $(wildcard $(COVER_DIR)/*.d)
