# ============================================================================
# Makefile for the Tether Compiler
# ============================================================================

# Compiler and flags
CXX       = g++
CC        = gcc
CXXFLAGS  = -std=c++17 -Wall -Wextra -O3 -march=native -flto -I include
CFLAGS    = -std=c11 -Wall -Wextra -O2

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

# C++ sources (compiler itself)
CXX_SRCS  = $(shell find $(SRCDIR) -name '*.cpp')
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
	$(CC) $(CFLAGS) -c -o $@ $<

# ============================================================================
# Build runtime static library
# ============================================================================

$(RUNTIME_LIB): $(C_OBJ)
	ar rcs $@ $<

# ============================================================================
# Dependency tracking (auto-generated .d files from -MMD -MP)
# ============================================================================

-include $(CXX_OBJS:.o=.d)

# ============================================================================
# Clean
# ============================================================================

.PHONY: clean
clean:
	rm -rf $(BUILDDIR) $(TARGET)

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
# Help
# ============================================================================

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
	@echo "  install     - Install to PREFIX (default: /usr/local)"
	@echo "  uninstall   - Remove installed files"
	@echo "  help        - Show this message"
	@echo ""
	@echo "Variables:"
	@echo "  CXX       - C++ compiler (default: g++)"
	@echo "  CC        - C compiler (default: gcc)"
	@echo "  CXXFLAGS  - C++ compiler flags"
	@echo "  CFLAGS    - C compiler flags"
	@echo "  PREFIX    - Install prefix (default: /usr/local)"
