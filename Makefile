# ChronoKV v25 build matrix — two-file project (chronokv.hpp + main.cpp).
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

CXX      ?= g++
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
ASAN_FLAGS    = -O1 -g -fsanitize=address,undefined \
                -fno-omit-frame-pointer -fno-sanitize-recover=undefined \
                -DCKV_UNDER_SANITIZER=1
TSAN_FLAGS    = -O1 -g -fsanitize=thread \
                -fno-omit-frame-pointer -DCKV_UNDER_SANITIZER=1
STRESS_FLAGS  = -O2 -g -DCHRONOKV_STRESS -DCKV_UNDER_SANITIZER=1

BIN_DIR = build

# ---- hooks-on targets ----
release: $(BIN_DIR)/release/test
asan:    $(BIN_DIR)/asan/test
tsan:    $(BIN_DIR)/tsan/test
stress:  $(BIN_DIR)/stress/test

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

# ---- hooks-on build rules ----
$(BIN_DIR)/release/test: main.cpp chronokv.hpp | $(BIN_DIR)/release
        $(CXX) $(CXXSTD) $(WARN) $(INCLUDE) $(HOOKS_ON_DEFS) $(CKV_EXTRA_DEFS) $(RELEASE_FLAGS) \
            main.cpp -o $@ -lpthread
$(BIN_DIR)/asan/test: main.cpp chronokv.hpp | $(BIN_DIR)/asan
        $(CXX) $(CXXSTD) $(WARN) $(INCLUDE) $(HOOKS_ON_DEFS) $(CKV_EXTRA_DEFS) $(ASAN_FLAGS) \
            main.cpp -o $@ -lpthread
$(BIN_DIR)/tsan/test: main.cpp chronokv.hpp | $(BIN_DIR)/tsan
        $(CXX) $(CXXSTD) $(WARN) $(INCLUDE) $(HOOKS_ON_DEFS) $(CKV_EXTRA_DEFS) $(TSAN_FLAGS) \
            main.cpp -o $@ -lpthread
$(BIN_DIR)/stress/test: main.cpp chronokv.hpp | $(BIN_DIR)/stress
        $(CXX) $(CXXSTD) $(WARN) $(INCLUDE) $(HOOKS_ON_DEFS) $(CKV_EXTRA_DEFS) $(STRESS_FLAGS) \
            main.cpp -o $@ -lpthread

# ---- hooks-off build rules ----
$(BIN_DIR)/smoke_off/release/test: main.cpp chronokv.hpp | $(BIN_DIR)/smoke_off/release
        $(CXX) $(CXXSTD) $(WARN) $(INCLUDE) $(HOOKS_OFF_DEFS) $(CKV_EXTRA_DEFS) $(RELEASE_FLAGS) \
            main.cpp -o $@ -lpthread
$(BIN_DIR)/smoke_off/asan/test: main.cpp chronokv.hpp | $(BIN_DIR)/smoke_off/asan
        $(CXX) $(CXXSTD) $(WARN) $(INCLUDE) $(HOOKS_OFF_DEFS) $(CKV_EXTRA_DEFS) $(ASAN_FLAGS) \
            main.cpp -o $@ -lpthread
$(BIN_DIR)/smoke_off/tsan/test: main.cpp chronokv.hpp | $(BIN_DIR)/smoke_off/tsan
        $(CXX) $(CXXSTD) $(WARN) $(INCLUDE) $(HOOKS_OFF_DEFS) $(CKV_EXTRA_DEFS) $(TSAN_FLAGS) \
            main.cpp -o $@ -lpthread
$(BIN_DIR)/smoke_off/stress/test: main.cpp chronokv.hpp | $(BIN_DIR)/smoke_off/stress
        $(CXX) $(CXXSTD) $(WARN) $(INCLUDE) $(HOOKS_OFF_DEFS) $(CKV_EXTRA_DEFS) $(STRESS_FLAGS) \
            main.cpp -o $@ -lpthread

# ---- dirs ----
$(BIN_DIR)/release $(BIN_DIR)/asan $(BIN_DIR)/tsan $(BIN_DIR)/stress \
$(BIN_DIR)/smoke_off/release $(BIN_DIR)/smoke_off/asan \
$(BIN_DIR)/smoke_off/tsan $(BIN_DIR)/smoke_off/stress:
        mkdir -p $@

clean:
        rm -rf $(BIN_DIR)

.PHONY: release asan tsan stress \
        smoke_off_release smoke_off_asan smoke_off_tsan smoke_off_stress \
        all clean
