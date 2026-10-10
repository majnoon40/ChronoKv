# ChronoKV build matrix — two-file project (chronokv.hpp + main.cpp).
# (v28 doc fix: header said "v25" while the product shipped 0.27+.)
# v25.1 M2 close: io_uring wrapper merged into chronokv.hpp at
# [SECTION_2B_IO_URING]; chronokv_iouring.hpp no longer exists.
#
# Build modes:
#   hooks-on  (-DCHRONOKV_TEST_HOOKS): full test suite (engine tests +
#                                       B+ tree fuzz + page pool + latency).
#   hooks-off (no -DCHRONOKV_TEST_HOOKS): public-API smoke only.
#
# Both modes built across the four-config sanitizer matrix.
# No separate bench/ directory or extra .cpp files — everything is in
# chronokv.hpp + main.cpp.
#
# Common variables (override on the command line, e.g. `make asan CXX=g++-13`):
#   CXX           compiler (default g++)
#   CKV_EXTRA_DEFS extra -D flags appended to every build (see the
#                 CKV_IOURING_DISABLED note below)
#
# v25.2: the TSan batch split (TSAN_BATCH / CHRONOKV_TSAN_BATCH) was
# removed — `make tsan` runs the FULL suite as one step. CI runners
# complete it comfortably inside one job.

# NOTE (v29 M2 item 5, 2026-10-10): the test suite is now FOUR translation
# units - main.cpp (~10.6k lines) plus tests/tests_{dst,remediation,lincheck}
# - each including the ~13.5k-line header; the largest cc1plus unit is
# main.cpp at ~24k lines, down from the 28k monolith that a 1 GiB container
# could not build even at bare -O0. Verified on the 1 GiB confirmation
# sandbox after the split: release -O1 with NO ggc workaround (the old
# monolith was OOM-killed there). -O2 on small containers can still be
# tight; fallbacks in order: RELEASE_FLAGS="-O1 -g", then
# RELEASE_FLAGS="-O0 --param ggc-min-expand=5". The engine-side split with
# the amalgamated release artifact remains v30 M2.
CXX      ?= g++

# v29: gcc spells the static sanitizer-runtime flag -static-libasan; clang
# rejects it ("unknown argument") and spells it -static-libsan. Pick by $(CXX)
# so `make asan CXX=clang++-18` links (CXX is set on the line above, or on the
# command line, which overrides it).
ifneq (,$(findstring clang,$(CXX)))
ASAN_STATIC ?= -static-libsan
else
ASAN_STATIC ?= -static-libasan
endif
CXXSTD    = -std=c++20
WARN      = -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable \
            -Wno-unused-but-set-variable -Wno-sign-compare -Wno-missing-field-initializers \
            -Wno-self-move
INCLUDE   = -I.
SRC_DIR   = $(abspath .)

HOOKS_ON_DEFS  = -DCHRONOKV_TEST_HOOKS -DCHRONOKV_FAULT_INJECTION \
                 -DCHRONOKV_SOURCE_DIR=\"$(SRC_DIR)\"
HOOKS_OFF_DEFS =

RELEASE_FLAGS = -O2 -g
# v25.1 M2 close finding (corrected): the container's g++ is 14.2.0 (Debian
# 14.2.0-19), NOT 12.2.0 as the original M2 closing report claimed. Both
# native (g++ 15.2.0) and container (g++ 14.2.0) require -static-libasan
# to avoid "ASan runtime does not come first in initial library list" at
# process startup. Static linking is a strict superset of dynamic for
# ASan, so this is safe on all g++ versions.
ASAN_FLAGS    = -O1 -g -fsanitize=address,undefined \
                -fno-omit-frame-pointer -fno-sanitize-recover=undefined \
                -DCKV_UNDER_SANITIZER=1 $(ASAN_STATIC)
TSAN_FLAGS    = -O1 -g -fsanitize=thread \
                -fno-omit-frame-pointer -DCKV_UNDER_SANITIZER=1
# v26.2: -DCKV_UNDER_SANITIZER=1 was REMOVED from STRESS_FLAGS. It had
# always been silently ineffective: the header's sanitizer auto-detection
# redefined the macro to 0 (with a "redefined" warning) because a stress
# build sets no __SANITIZE_* builtin. The header now honors command-line
# defines (#ifndef guard), so keeping the flag would have SUDDENLY flipped
# the stress job to the lighter sanitizer test variant (skipping the
# fork-based crash tests) — a coverage change nobody ever reviewed, after
# years of green runs at the effective value 0. Stress builds now get
# exactly what they always effectively had (auto-detect -> 0), warning-free.
# v27 M3: gcov-instrumented build for the coverage job. -O0 keeps gcov's
# line attribution exact; --coverage adds -fprofile-arcs -ftest-coverage
# (and must appear at link time too — see the coverage rule). No -g: the
# gcov text output this project parses needs no debug info, and dropping
# it keeps the build inside a 1 GiB container's RSS (with -g, cc1plus is
# OOM-killed there; runners would not care, but local verification must
# run the exact CI flags).
# CKV_COVERAGE_BUILD switches the __gcov_dump reference STRONG (a weak
# undefined symbol does not pull its member out of libgcov.a — run #21).
COVERAGE_FLAGS = -O0 --coverage -DCKV_COVERAGE_BUILD=1
STRESS_FLAGS  = -O2 -g -DCHRONOKV_STRESS

BIN_DIR = build
TSAN_BIN = $(BIN_DIR)/tsan/test

# ---- v29 M1: benchmark arena (bench/arena.cpp — hooks-OFF consumer build;
# the arena measures the product's real face: public API only, no test
# hooks. Override ARENA_FLAGS on small containers, e.g. ARENA_FLAGS="-O1".)
empty :=
space := $(empty) $(empty)
ARENA_FLAGS ?= -O2 -g
ARENA_INFO  := $(subst $(space),_,$(CXX) $(CXXSTD) $(ARENA_FLAGS))

# ---- hooks-on targets ----
release: $(BIN_DIR)/release/test
asan:    $(BIN_DIR)/asan/test
tsan:    $(TSAN_BIN)
stress:  $(BIN_DIR)/stress/test
coverage: $(BIN_DIR)/coverage/test

# ---- arena target (v29 M1) ----
arena: $(BIN_DIR)/arena/arena

$(BIN_DIR)/arena/arena: bench/arena.cpp chronokv.hpp | $(BIN_DIR)/arena
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDE) $(CKV_EXTRA_DEFS) \
	    -DARENA_BUILD_INFO=\"$(ARENA_INFO)\" $(ARENA_FLAGS) \
	    bench/arena.cpp -o $@ -lpthread

# ---- v29 M1 step 3: baseline adapters (bench/baselines.cpp) ----
# Rule 10: baselines are BENCHMARKS, NOT DEPENDENCIES — detection is by
# header presence; a missing library omits that engine from the binary.
# bench/baselines.cpp is the ONLY file in the repo that includes a
# third-party storage header. Pinning policy: bench/third_party/README.md.
BASE_DEFS :=
BASE_LIBS :=
ifneq ($(wildcard /usr/include/sqlite3.h),)
BASE_DEFS += -DCKV_BASE_SQLITE=1
BASE_LIBS += -lsqlite3
endif
ifneq ($(wildcard /usr/include/lmdb.h),)
BASE_DEFS += -DCKV_BASE_LMDB=1
BASE_LIBS += -llmdb
endif
ifneq ($(wildcard /usr/include/rocksdb/db.h),)
BASE_DEFS += -DCKV_BASE_ROCKSDB=1
BASE_LIBS += -lrocksdb
endif

arena-baselines: $(BIN_DIR)/arena/baselines

$(BIN_DIR)/arena/baselines: bench/baselines.cpp | $(BIN_DIR)/arena
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDE) $(BASE_DEFS) \
	    -DARENA_BUILD_INFO=\"$(ARENA_INFO)\" $(ARENA_FLAGS) \
	    bench/baselines.cpp -o $@ $(BASE_LIBS) -lpthread

# ---- hooks-off targets ----
smoke_off_release: $(BIN_DIR)/smoke_off/release/test
smoke_off_asan:    $(BIN_DIR)/smoke_off/asan/test
smoke_off_tsan:    $(BIN_DIR)/smoke_off/tsan/test
smoke_off_stress:  $(BIN_DIR)/smoke_off/stress/test

# ---- aggregate ----
all: release asan tsan stress smoke_off_release smoke_off_asan smoke_off_tsan smoke_off_stress

# v25.1 M2: CKV_IOURING_DISABLED is set ONLY for the container dev environment
# where seccomp blocks io_uring I/O ops (EPERM). On native hardware, do NOT
# define this — let the real io_uring path be attempted.
# The container build adds -DCKV_IOURING_DISABLED via the environment variable
# CKV_EXTRA_DEFS (default empty on native hardware).
CKV_EXTRA_DEFS ?=

# ---- test-suite build rules (v29 M2 item 5: FOUR translation units) ----
# main.cpp is no longer the whole suite: the lincheck, DST and remediation
# batteries live in tests/tests_*.cpp, cross-TU entry points declared in
# tests/test_decls.hpp (API-1's ODR-safe header is what made this split
# mechanical - every TU includes chronokv.hpp and links once). Objects sit
# FLAT in each mode dir so the coverage job's `gcov build/coverage/*.gcda`
# keeps working untouched. Per-mode flags are unchanged; the largest cc1plus
# unit is now main.cpp (~24k lines with the header), down from the 28k
# monolith. v27 M3 gcov note stands: --coverage at BOTH compile and link;
# .gcno/.gcda land next to the objects, flat in the mode dir.
TEST_SRCS = main.cpp tests/tests_dst.cpp tests/tests_remediation.cpp tests/tests_lincheck.cpp
CKV_HDRS  = chronokv.hpp tests/test_decls.hpp
TEST_OBJS_FOR = main.o tests_dst.o tests_remediation.o tests_lincheck.o

# $(call mk_test_bin,MODE_DIR,DEFS,FLAGS,LDEXTRA)
define mk_test_bin
$(BIN_DIR)/$(1)/main.o: main.cpp $(CKV_HDRS) | $(BIN_DIR)/$(1)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDE) $(2) $(CKV_EXTRA_DEFS) $(3) -c main.cpp -o $$@
$(BIN_DIR)/$(1)/tests_%.o: tests/tests_%.cpp $(CKV_HDRS) | $(BIN_DIR)/$(1)
	$(CXX) $(CXXSTD) $(WARN) $(INCLUDE) $(2) $(CKV_EXTRA_DEFS) $(3) -c $$< -o $$@
$(BIN_DIR)/$(1)/test: $(addprefix $(BIN_DIR)/$(1)/,$(TEST_OBJS_FOR)) | $(BIN_DIR)/$(1)
	$(CXX) $(CXXSTD) $(WARN) $(3) $$^ -o $$@ -lpthread $(4)
endef

$(eval $(call mk_test_bin,release,$(HOOKS_ON_DEFS),$(RELEASE_FLAGS),))
$(eval $(call mk_test_bin,asan,$(HOOKS_ON_DEFS),$(ASAN_FLAGS),))
$(eval $(call mk_test_bin,tsan,$(HOOKS_ON_DEFS),$(TSAN_FLAGS),))
$(eval $(call mk_test_bin,stress,$(HOOKS_ON_DEFS),$(STRESS_FLAGS),))
$(eval $(call mk_test_bin,coverage,$(HOOKS_ON_DEFS),$(COVERAGE_FLAGS),--coverage))
$(eval $(call mk_test_bin,smoke_off/release,$(HOOKS_OFF_DEFS),$(RELEASE_FLAGS),))
$(eval $(call mk_test_bin,smoke_off/asan,$(HOOKS_OFF_DEFS),$(ASAN_FLAGS),))
$(eval $(call mk_test_bin,smoke_off/tsan,$(HOOKS_OFF_DEFS),$(TSAN_FLAGS),))
$(eval $(call mk_test_bin,smoke_off/stress,$(HOOKS_OFF_DEFS),$(STRESS_FLAGS),))

# ---- v29 CI hardening: fast static gates (no codegen, no test run) ----
# -fsyntax-only parses and type-checks without generating code, so these need
# none of the >1 GiB cc1plus RSS of a real build and take seconds. They are
# what CI's `lint` job runs, and they run locally the same way.
#   lint-werror      the project's own WARN set promoted to -Werror, over the
#                    header alone (hooks-off and hooks-on) and the whole test
#                    suite (main.cpp, hooks-on and hooks-off). Zero warnings
#                    today, so this is free; it keeps it that way. Try it with
#                    another compiler:  make lint-werror CXX=clang++-18
#   lint-conversion  -Wconversion ratchet over the header: the warning count may
#                    not grow past ci/wconversion.baseline (scripts/
#                    lint_wconversion.sh). The uint16_t narrowing class behind
#                    audit CKV-001/002 is exactly what it flags.
LINT_WARN = $(WARN) -Werror

lint: lint-werror lint-conversion lint-header

lint-werror:
	printf '#include "chronokv.hpp"\n' | $(CXX) $(CXXSTD) $(LINT_WARN) $(INCLUDE) $(HOOKS_OFF_DEFS) $(CKV_EXTRA_DEFS) -fsyntax-only -x c++ -
	printf '#include "chronokv.hpp"\n' | $(CXX) $(CXXSTD) $(LINT_WARN) $(INCLUDE) $(HOOKS_ON_DEFS) $(CKV_EXTRA_DEFS) -fsyntax-only -x c++ -
	$(CXX) $(CXXSTD) $(LINT_WARN) $(INCLUDE) $(HOOKS_ON_DEFS) $(CKV_EXTRA_DEFS) -fsyntax-only main.cpp tests/tests_dst.cpp tests/tests_remediation.cpp tests/tests_lincheck.cpp
	$(CXX) $(CXXSTD) $(LINT_WARN) $(INCLUDE) $(HOOKS_OFF_DEFS) $(CKV_EXTRA_DEFS) -fsyntax-only main.cpp tests/tests_dst.cpp tests/tests_remediation.cpp tests/tests_lincheck.cpp

lint-conversion:
	CXX='$(CXX)' bash scripts/lint_wconversion.sh

# Header hygiene (audit API-1): the "single header" must be usable the way any
# real consumer uses it (#pragma once + inline statics make both checks pass).
#   lint-header-reinclude  the header included twice in one translation unit
#   lint-header-2tu        the header included from two TUs that are linked
LH = $(BIN_DIR)/lint-header

lint-header: lint-header-reinclude lint-header-2tu

lint-header-reinclude:
	printf '#include "chronokv.hpp"\n#include "chronokv.hpp"\n' | $(CXX) $(CXXSTD) $(INCLUDE) -fsyntax-only -x c++ -

lint-header-2tu:
	mkdir -p $(LH)
	printf '#include "chronokv.hpp"\nint ckv_tu1() { return 1; }\n' > $(LH)/tu1.cpp
	printf '#include "chronokv.hpp"\nint ckv_tu2() { return 2; }\n' > $(LH)/tu2.cpp
	printf 'int ckv_tu1(); int ckv_tu2();\nint main() { return ckv_tu1() + ckv_tu2() == 3 ? 0 : 1; }\n' > $(LH)/main.cpp
	$(CXX) $(CXXSTD) $(INCLUDE) -O0 -pthread $(LH)/tu1.cpp $(LH)/tu2.cpp $(LH)/main.cpp -o $(LH)/a.out
	$(LH)/a.out
	@# v29 M2 item 5: the STRESS-defs 2-TU link — the shape the plain leg missed
	@# (dst::my_tid was a non-inline namespace-scope thread_local; only STRESS
	@# builds define it, and only a multi-TU link rejects the duplicates).
	$(CXX) $(CXXSTD) $(INCLUDE) -O0 -pthread -DCHRONOKV_STRESS $(LH)/tu1.cpp $(LH)/tu2.cpp $(LH)/main.cpp -o $(LH)/a.out.stress
	$(LH)/a.out.stress

# ---- dirs ----
$(BIN_DIR)/release $(BIN_DIR)/asan $(BIN_DIR)/tsan $(BIN_DIR)/stress $(BIN_DIR)/coverage \
$(BIN_DIR)/arena \
$(BIN_DIR)/smoke_off/release $(BIN_DIR)/smoke_off/asan \
$(BIN_DIR)/smoke_off/tsan $(BIN_DIR)/smoke_off/stress:
	mkdir -p $@

clean:
	rm -rf $(BIN_DIR)

.PHONY: release asan tsan stress coverage arena arena-baselines \
	smoke_off_release smoke_off_asan smoke_off_tsan smoke_off_stress \
	lint lint-werror lint-conversion lint-header lint-header-reinclude lint-header-2tu \
	all clean
