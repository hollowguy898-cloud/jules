# ============================================================================
# Makefile for the Tether Compiler
# ============================================================================

# Compiler and flags
CXX       = g++
CC        = gcc
CXXFLAGS  = -std=c++17 -Wall -Wextra -O3 -march=native -flto -I include
# NOTE: The C runtime is compiled WITHOUT -flto when using GCC because GCC
# LTO bytecode in the runtime .a is incompatible with LLVM and causes link
# errors when the linker tries to perform cross-language LTO.
# However, if clang is available, we compile the runtime WITH -flto using
# clang so that the runtime can participate in cross-language LTO with the
# Tether-generated LLVM IR. This allows LLVM to inline runtime functions
# (tether_yield, tether_box_new, etc.) and eliminate dead code.
# To force clang for the runtime: make RUNTIME_CC=clang
RUNTIME_CC ?= $(shell which clang 2>/dev/null)
ifeq ($(RUNTIME_CC),)
  # clang not found — use gcc without -flto (safe fallback)
  RUNTIME_CC = $(CC)
  CFLAGS    = -std=c11 -Wall -Wextra -O3 -march=native
else
  # clang available — compile runtime with -flto for cross-language optimization
  CFLAGS    = -std=c11 -Wall -Wextra -O3 -march=native -flto
endif

# mimalloc — fast allocator (2-3x faster than glibc malloc)
# Automatically detected: if libmimalloc is installed, use it.
# To disable: make CFLAGS="-DTETHER_NO_MIMALLOC" ...
MIMALLOC_CFLAGS  := $(shell pkg-config --cflags mimalloc 2>/dev/null)
MIMALLOC_LDFLAGS := $(shell pkg-config --libs mimalloc 2>/dev/null)
ifneq ($(MIMALLOC_LDFLAGS),)
CFLAGS   += $(MIMALLOC_CFLAGS) -DTETHER_USE_MIMALLOC
LDFLAGS  += $(MIMALLOC_LDFLAGS)
endif

# Directories
SRCDIR    = src
INCDIR    = include
BUILDDIR  = build
RUNTIMEDIR = runtime
TESTDIR   = tests

# Target executable
TARGET    = tetherc

# ============================================================================
# Discover source files
# ============================================================================

# C++ sources (compiler itself - excludes pass/ which requires LLVM headers)
CXX_SRCS  = $(shell find $(SRCDIR) -name '*.cpp' -not -path '$(SRCDIR)/pass/*')
CXX_OBJS  = $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(CXX_SRCS))

# Library objects (everything EXCEPT main.cpp — for linking into benchmarks)
LIB_OBJS  = $(filter-out $(BUILDDIR)/driver/main.o,$(CXX_OBJS))

# C sources (runtime library)
C_SRCS    = $(RUNTIMEDIR)/tether_runtime.c
C_OBJ     = $(BUILDDIR)/runtime/tether_runtime.o

# Runtime static library
RUNTIME_LIB = $(BUILDDIR)/libtether_runtime.a

# ============================================================================
# Default target
# ============================================================================

.PHONY: all
all: $(TARGET) $(RUNTIME_LIB)

# ============================================================================
# Link the compiler executable
# ============================================================================

$(TARGET): $(CXX_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

# ============================================================================
# Compile C++ sources
# ============================================================================

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -MMD -MP -o $@ $<

# ============================================================================
# Compile C runtime
# ============================================================================

$(BUILDDIR)/runtime/%.o: $(RUNTIMEDIR)/%.c
	@mkdir -p $(dir $@)
	$(RUNTIME_CC) $(CFLAGS) -c -o $@ $<

# ============================================================================
# Build runtime static library
# ============================================================================

$(RUNTIME_LIB): $(C_OBJ)
	ar rcs $@ $<

# ============================================================================
# LLVM Pass Plugin — TetherAttrPass
#
# Custom LLVM pass plugin that reads Tether-specific metadata from the
# LLVM IR module and injects corresponding LLVM optimization attributes.
# This is loaded by clang via -fpass-plugin.
#
# Prerequisites: LLVM development headers (llvm-config must be on PATH)
# If LLVM is not available, the plugin build is skipped gracefully —
# the Tether compiler still works, just without the pass plugin.
# ============================================================================

PASS_PLUGIN = build/TetherAttrPass.so
LLVM_CONFIG ?= $(shell which llvm-config-18 2>/dev/null || which llvm-config-17 2>/dev/null || which llvm-config-16 2>/dev/null || which llvm-config-15 2>/dev/null || which llvm-config 2>/dev/null)

# Only build the pass plugin if llvm-config is available
ifneq ($(LLVM_CONFIG),)
PASS_PLUGIN_DEPS = src/pass/TetherAttrPass.cpp src/pass/PassPluginMain.cpp include/pass/TetherAttrPass.h

$(PASS_PLUGIN): $(PASS_PLUGIN_DEPS)
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -shared -fPIC -O2 $(shell $(LLVM_CONFIG) --cxxflags) -I include -o $@ src/pass/TetherAttrPass.cpp src/pass/PassPluginMain.cpp $(shell $(LLVM_CONFIG) --ldflags --libs core)
else
# llvm-config not found - create a stub target that warns
$(PASS_PLUGIN):
	@echo "Warning: llvm-config not found. Skipping TetherAttrPass plugin build."
	@echo "         The compiler will work without the pass plugin (metadata will"
	@echo "         be emitted but not consumed by LLVM optimization passes)."
endif

.PHONY: pass-plugin
pass-plugin: $(PASS_PLUGIN)

# ============================================================================
# Dependency tracking (auto-generated .d files from -MMD -MP)
# ============================================================================

-include $(CXX_OBJS:.o=.d)

# ============================================================================
# Clean
# ============================================================================

.PHONY: clean
clean:
	rm -rf $(BUILDDIR) $(TARGET) $(PASS_PLUGIN)

# ============================================================================
# Test target
# ============================================================================

.PHONY: test
test: $(TARGET)
	@echo "=== Testing compiler on examples ==="
	@for f in examples/*.tth; do \
		echo "  Compiling $$f ..."; \
                ./$(TARGET) --emit-ir -o /tmp/test_output.ll $$f 2>&1 || echo "  FAILED: $$f"; \
	done
	@echo "=== Tests complete ==="

# ============================================================================
# Install
# ============================================================================

PREFIX ?= /usr/local

.PHONY: install
install: $(TARGET) $(RUNTIME_LIB)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/tetherc
	install -d $(DESTDIR)$(PREFIX)/lib
	install -m 644 $(RUNTIME_LIB) $(DESTDIR)$(PREFIX)/lib/libtether_runtime.a
	install -d $(DESTDIR)$(PREFIX)/include/tether
	install -m 644 $(RUNTIMEDIR)/tether_runtime.h $(DESTDIR)$(PREFIX)/include/tether/tether_runtime.h

# ============================================================================
# Uninstall
# ============================================================================

.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/tetherc
	rm -f $(DESTDIR)$(PREFIX)/lib/libtether_runtime.a
	rm -f $(DESTDIR)$(PREFIX)/include/tether/tether_runtime.h

# ============================================================================
# Benchmarks
# ============================================================================

# Speed benchmark (compiler phase throughput)
BENCH_SPEED = build/bench_speed
$(BENCH_SPEED): benchmarks/speed_benchmark.cpp $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -O2 -o $@ $< $(LIB_OBJS)

# Real-workload benchmark (compile real .tth programs)
BENCH_REAL = build/bench_real
$(BENCH_REAL): benchmarks/real_workload_bench.cpp $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -O2 -o $@ $< $(LIB_OBJS)

# ECS SoA vs AoS benchmark (standalone C++ runtime benchmark)
BENCH_ECS = build/bench_ecs
$(BENCH_ECS): benchmarks/ecs/tether_ecs.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O3 -march=native -o $@ $< -lm

.PHONY: bench
bench: $(BENCH_SPEED) $(BENCH_REAL) $(BENCH_ECS) $(TARGET)
	@echo "=============================================="
	@echo "  Tether Compiler Benchmark Suite"
	@echo "=============================================="
	@echo ""
	@echo "--- 1. Compiler Phase Throughput ---"
	@$(BENCH_SPEED)
	@echo ""
	@echo "--- 2. Real Workload Compilation ---"
	@$(BENCH_REAL)
	@echo ""
	@echo "--- 3. ECS SoA vs AoS Runtime ---"
	@$(BENCH_ECS)
	@echo ""
	@echo "--- 4. End-to-End Compilation ---"
	@for f in benchmarks/real_workload/*.tth; do \
		echo "  Compiling $$f (-O3) ..."; \
                ./$(TARGET) -O3 --emit-ir -v "$$f" -o /tmp/bench_out.ll 2>&1 | head -5; \
		echo "  IR size: $$(wc -c < /tmp/bench_out.ll) bytes"; \
		echo ""; \
	done
	@echo "=============================================="
	@echo "  Benchmark Suite Complete"
	@echo "=============================================="

.PHONY: bench-speed
bench-speed: $(BENCH_SPEED)
	@$(BENCH_SPEED)

.PHONY: bench-real
bench-real: $(BENCH_REAL)
	@$(BENCH_REAL)

.PHONY: bench-ecs
bench-ecs: $(BENCH_ECS)
	@$(BENCH_ECS)

# ============================================================================
# Propeller — Profile-Guided Code Layout Optimization
# ============================================================================
#
# The Propeller pipeline optimizes I-cache hit rates, branch prediction, and
# code layout by:
#   1. Building an instrumented binary (IR-level PGO)
#   2. Collecting runtime profiles with representative workloads
#   3. Rebuilding with PGO feedback + BB section reordering + lld linking
#
# Prerequisites: LLVM 19 with lld, llvm-profdata (set LLVM_BIN below)
# ============================================================================

LLVM_BIN  ?= /tmp/my-project/llvm-bin/LLVM-19.1.7-Linux-X64/bin
PROPELLER ?= scripts/propeller_build.sh

# Propeller: full pipeline (instrument → profile → optimize)
# Usage: make propeller-full INPUT=benchmarks/propeller_bench.tth [ARGS=]
.PHONY: propeller-full
propeller-full: $(TARGET)
	@bash $(PROPELLER) full $(INPUT) $(ARGS)

# Propeller: instrument only (Phase 1)
# Usage: make propeller-instrument INPUT=benchmarks/propeller_bench.tth
.PHONY: propeller-instrument
propeller-instrument: $(TARGET)
	@bash $(PROPELLER) instrument $(INPUT)

# Propeller: collect profiles (Phase 2)
# Usage: make propeller-profile BINARY=/tmp/propeller_bench_inst
.PHONY: propeller-profile
propeller-profile: $(TARGET)
	@bash $(PROPELLER) profile $(BINARY) $(ARGS)

# Propeller: rebuild with profiles (Phase 3)
# Usage: make propeller-optimize INPUT=benchmarks/propeller_bench.tth
.PHONY: propeller-optimize
propeller-optimize: $(TARGET)
	@bash $(PROPELLER) optimize $(INPUT)

# Propeller: benchmark baseline vs PGO vs Propeller
# Usage: make propeller-bench INPUT=benchmarks/propeller_bench.tth
.PHONY: propeller-bench
propeller-bench: $(TARGET)
	@bash $(PROPELLER) bench $(INPUT) $(ARGS)

# Propeller: clean temp files
.PHONY: propeller-clean
propeller-clean:
	@bash $(PROPELLER) clean

# ============================================================================
# PGO (Profile-Guided Optimization) targets
# ============================================================================

# One-command PGO cycle
# Usage: make pgo SOURCE=benchmarks/fibonacci.tth
# Optional: ARGS="--iterations 1000" to pass arguments to the benchmark
.PHONY: pgo
pgo: $(TARGET)
	@bash scripts/pgo.sh $(SOURCE) $(ARGS)

# ============================================================================
# Help
# ============================================================================

.PHONY: setup
setup:
	git config core.hooksPath .githooks
	@echo "Git hooks configured. Pre-commit hook will block leaked file patterns."

.PHONY: help
help:
	@echo "Tether Compiler Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all         - Build the compiler and runtime (default)"
	@echo "  clean       - Remove build artifacts"
	@echo "  test        - Build and run tests"
	@echo "  bench       - Run all benchmarks"
	@echo "  bench-speed - Run compiler phase throughput benchmark"
	@echo "  bench-real  - Run real-workload compilation benchmark"
	@echo "  bench-ecs   - Run ECS SoA vs AoS runtime benchmark"
	@echo "  pass-plugin - Build the TetherAttrPass LLVM plugin (requires LLVM dev headers)"
	@echo "  install     - Install to PREFIX (default: /usr/local)"
	@echo "  uninstall   - Remove installed files"
	@echo "  setup       - Configure git hooks (run after clone)"
	@echo "  help        - Show this message"
	@echo ""
	@echo "Propeller (PGO + Code Layout Optimization):"
	@echo "  propeller-full      - Full pipeline: instrument → profile → optimize"
	@echo "  propeller-instrument- Phase 1: Build instrumented binary"
	@echo "  propeller-profile   - Phase 2: Collect runtime profiles"
	@echo "  propeller-optimize  - Phase 3: Rebuild with PGO + BB sections + lld"
	@echo "  propeller-bench     - Benchmark: baseline vs PGO vs Propeller"
	@echo "  propeller-clean     - Clean Propeller temp files"
	@echo ""
	@echo "PGO (Profile-Guided Optimization):"
	@echo "  pgo                 - One-command PGO cycle: instrument -> profile -> optimize"
	@echo "                        Usage: make pgo SOURCE=benchmarks/propeller_bench.tth"
	@echo ""
	@echo "Variables:"
	@echo "  CXX       - C++ compiler (default: g++)"
	@echo "  CC        - C compiler (default: gcc)"
	@echo "  CXXFLAGS  - C++ compiler flags"
	@echo "  CFLAGS    - C compiler flags"
	@echo "  PREFIX    - Install prefix (default: /usr/local)"
