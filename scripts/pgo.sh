#!/bin/bash
# =============================================================================
# Tether Profile-Guided Optimization (PGO) — One-command cycle
#
# Usage: ./scripts/pgo.sh <source.tth> [benchmark_args...]
#
# This script:
# 1. Instruments the binary for profiling
# 2. Runs the benchmark to collect profile data
# 3. Recompiles with profile-guided optimization
# 4. (Optional) Applies Propeller code layout optimization
#
# Requires: clang with LLVM 18+, lld (optional, for Propeller)
# =============================================================================

set -e

SOURCE="$1"
shift || true
BENCHMARK_ARGS="$@"

if [ -z "$SOURCE" ]; then
    echo "Usage: $0 <source.tth> [benchmark_args...]"
    echo ""
    echo "This script performs a full PGO (Profile-Guided Optimization) cycle:"
    echo "  1. Instruments the binary for profiling"
    echo "  2. Runs the benchmark to collect profile data"
    echo "  3. Recompiles with profile-guided optimization"
    echo "  4. (Optional) Applies Propeller code layout optimization"
    echo ""
    echo "Requires: clang with LLVM 18+"
    echo "Optional: lld (for Propeller code layout optimization)"
    exit 1
fi

STEM=$(basename "$SOURCE" .tth)
PGO_DIR=".pgo-${STEM}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "[pgo] === Tether Profile-Guided Optimization ==="
echo "[pgo] Source: $SOURCE"
echo "[pgo] Profile dir: $PGO_DIR"

# Clean up any previous PGO data
rm -rf "$PGO_DIR"
mkdir -p "$PGO_DIR"

# Step 1: Build the Tether compiler (if not already built)
echo "[pgo] Step 1: Building Tether compiler..."
make -C "$PROJECT_DIR" tetherc RUNTIME_LIB -j$(nproc) 2>&1 | tail -1

# Step 2: Compile with instrumentation
echo "[pgo] Step 2: Compiling with profiling instrumentation..."
"$PROJECT_DIR/tetherc" "$SOURCE" -O3 --profile-generate -o "$PGO_DIR/${STEM}.instrumented"

# Step 3: Run the benchmark to collect profile data
echo "[pgo] Step 3: Running benchmark to collect profile data..."
LLVM_PROFILE_FILE="$PGO_DIR/default.profraw" \
    "$PGO_DIR/${STEM}.instrumented" $BENCHMARK_ARGS || true

# Step 4: Merge profile data
echo "[pgo] Step 4: Merging profile data..."
PROFDATA=""
if command -v llvm-profdata-19 &> /dev/null; then
    PROFDATA=llvm-profdata-19
elif command -v llvm-profdata-18 &> /dev/null; then
    PROFDATA=llvm-profdata-18
elif command -v llvm-profdata-17 &> /dev/null; then
    PROFDATA=llvm-profdata-17
elif command -v llvm-profdata &> /dev/null; then
    PROFDATA=llvm-profdata
else
    echo "[pgo] ERROR: llvm-profdata not found. Install LLVM 18+."
    echo "[pgo] You can install it with: apt install llvm-18"
    exit 1
fi

echo "[pgo] Using: $PROFDATA"
$PROFDATA merge -o "$PGO_DIR/default.profdata" "$PGO_DIR"/default.profraw 2>/dev/null || {
    # If there are no profraw files, warn but continue
    echo "[pgo] WARNING: No profile data collected (binary may have exited early)."
    echo "[pgo] Creating empty profile data..."
    $PROFDATA merge -o "$PGO_DIR/default.profdata" /dev/null 2>/dev/null || true
}

# Step 5: Recompile with profile-guided optimization
echo "[pgo] Step 5: Recompiling with PGO..."
"$PROJECT_DIR/tetherc" "$SOURCE" -O3 "--profile-use=$PGO_DIR/default.profdata" -o "$PGO_DIR/${STEM}.pgo"

# Step 6: Optional Propeller optimization (requires LLVM 19 + lld)
echo "[pgo] Step 6: Checking for Propeller support..."
HAS_PROPELLER=false
if command -v ld.lld-19 &> /dev/null; then
    HAS_PROPELLER=true
elif command -v lld-19 &> /dev/null; then
    HAS_PROPELLER=true
elif command -v ld.lld &> /dev/null; then
    # Check if it's lld 19+
    LLD_VERSION=$(ld.lld --version 2>/dev/null | head -1 | rg -o '[0-9]+\.[0-9]+' | head -1 || echo "0.0")
    LLD_MAJOR=$(echo "$LLD_VERSION" | cut -d. -f1)
    if [ "$LLD_MAJOR" -ge 19 ] 2>/dev/null; then
        HAS_PROPELLER=true
    fi
fi

if $HAS_PROPELLER; then
    echo "[pgo] Propeller support detected. Applying code layout optimization..."
    if [ -f "$PROJECT_DIR/scripts/propeller_build.sh" ]; then
        bash "$PROJECT_DIR/scripts/propeller_build.sh" full "$SOURCE" $BENCHMARK_ARGS || {
            echo "[pgo] WARNING: Propeller optimization failed. PGO binary is still available."
        }
    else
        echo "[pgo] propeller_build.sh not found. Skipping Propeller."
    fi
else
    echo "[pgo] Propeller not available (requires LLVM 19 + lld). Skipping."
fi

echo "[pgo] === PGO Complete ==="
echo "[pgo] Optimized binary: $PGO_DIR/${STEM}.pgo"
echo "[pgo] Profile data: $PGO_DIR/default.profdata"
echo "[pgo]"
echo "[pgo] To use the optimized binary, run:"
echo "[pgo]   $PGO_DIR/${STEM}.pgo [args...]"
