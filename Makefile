# ============================================================================
# Makefile for the Jules Compiler
# ============================================================================

# Compiler and flags
CXX       = g++
CC        = gcc
CXXFLAGS  = -std=c++17 -Wall -Wextra -O2 -I include
CFLAGS    = -std=c11 -Wall -Wextra -O2

# Directories
SRCDIR    = src
INCDIR    = include
BUILDDIR  = build
RUNTIMEDIR = runtime
TESTDIR   = tests

# Target executable
TARGET    = julesc

# ============================================================================
# Discover source files
# ============================================================================

# C++ sources (compiler itself)
CXX_SRCS  = $(shell find $(SRCDIR) -name '*.cpp')
CXX_OBJS  = $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(CXX_SRCS))

# C sources (runtime library)
C_SRCS    = $(RUNTIMEDIR)/jules_runtime.c
C_OBJ     = $(BUILDDIR)/runtime/jules_runtime.o

# Runtime static library
RUNTIME_LIB = $(BUILDDIR)/libjules_runtime.a

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
	@for f in examples/*.jl; do \
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
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/julesc
	install -d $(DESTDIR)$(PREFIX)/lib
	install -m 644 $(RUNTIME_LIB) $(DESTDIR)$(PREFIX)/lib/libjules_runtime.a
	install -d $(DESTDIR)$(PREFIX)/include/jules
	install -m 644 $(RUNTIMEDIR)/jules_runtime.h $(DESTDIR)$(PREFIX)/include/jules/jules_runtime.h

# ============================================================================
# Uninstall
# ============================================================================

.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/julesc
	rm -f $(DESTDIR)$(PREFIX)/lib/libjules_runtime.a
	rm -f $(DESTDIR)$(PREFIX)/include/jules/jules_runtime.h

# ============================================================================
# Help
# ============================================================================

.PHONY: help
help:
	@echo "Jules Compiler Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all       - Build the compiler and runtime (default)"
	@echo "  clean     - Remove build artifacts"
	@echo "  test      - Build and run tests"
	@echo "  install   - Install to PREFIX (default: /usr/local)"
	@echo "  uninstall - Remove installed files"
	@echo "  help      - Show this message"
	@echo ""
	@echo "Variables:"
	@echo "  CXX       - C++ compiler (default: g++)"
	@echo "  CC        - C compiler (default: gcc)"
	@echo "  CXXFLAGS  - C++ compiler flags"
	@echo "  CFLAGS    - C compiler flags"
	@echo "  PREFIX    - Install prefix (default: /usr/local)"
