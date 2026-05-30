# Tether

A high-performance systems programming language that compiles through LLVM, designed for workloads where every nanosecond counts — HFT engines, game loops, real-time DSP, and HPC kernels.

Tether gives you low-level control with modern ergonomics: `pure` functions for guaranteed side-effect-free optimization, `noalloc` for zero-heap guarantees, `soa struct` for cache-friendly data layouts, Zig-style error unions, and a borrow checker — all emitting LLVM IR that goes head-to-head with C on compute benchmarks.

## Quick Start

### Prerequisites

- **C++17 compiler** — g++ 9+ or clang++ 10+
- **C11 compiler** — gcc or clang
- **clang** — used as the backend for compiling and linking (searched as clang-19 through clang)
- **make**

Optional:
- **LLVM development headers** (`llvm-config`) — for building the TetherAttrPass LLVM plugin
- **mimalloc** (`libmimalloc`) — auto-detected for 2-3x faster allocation
- **Python 3** — for benchmark runner scripts

### Build

```bash
git clone https://github.com/hollowguy898-cloud/jules.git
cd jules
make
```

This produces two artifacts:
- `tetherc` — the compiler binary
- `build/libtether_runtime.a` — the runtime static library

### Hello World

Create `hello.tth`:

```tether
fn main() i32 {
    return 0;
}
```

Compile and run:

```bash
./tetherc hello.tth        # produces ./hello
./hello; echo $?            # prints: 0
```

### Install

```bash
make install                # installs to /usr/local by default
make install PREFIX=~/.local  # custom prefix
```

This installs:
- `tetherc` → `<PREFIX>/bin/tetherc`
- `libtether_runtime.a` → `<PREFIX>/lib/libtether_runtime.a`
- `tether_runtime.h` → `<PREFIX>/include/tether/tether_runtime.h`

Uninstall with `make uninstall`.

## Usage

```
Usage: tetherc [options] <input.tth>

Options:
  -o <output>         Output file path
  -O0                 No optimization (default)
  -O1                 Basic optimization
  -O2                 Standard optimization
  -O3                 Aggressive optimization
  -Os                 Optimize for size
  -Oz                 Aggressive size optimization
  --emit-obj          Emit an object file (.o)
  --emit-asm          Emit assembly (.s)
  --emit-ir           Emit LLVM IR (.ll)
  --emit-exe          Emit an executable (default)
  --profile-generate  Generate PGO profile data
  --profile-use <path> Use PGO profile data
  --target <triple>   Cross-compile target (x86_64, aarch64, riscv64, wasm32)
  -v                  Verbose mode (print each compilation phase)
  --help              Show help message
```

### Examples

```bash
tetherc hello.tth                       # Compile to executable
tetherc -O3 -o bench bench.tth          # Aggressive optimization
tetherc --emit-ir hello.tth             # Emit LLVM IR to hello.ll
tetherc --emit-asm -v hello.tth         # Emit assembly with verbose output
tetherc --target aarch64 hello.tth      # Cross-compile for AArch64
tetherc --profile-generate bench.tth    # PGO instrumentation
```

## Language Guide

### Variables

Tether uses `val` for immutable bindings and `var` for mutable ones:

```tether
val x: i32 = 42;       // immutable — cannot be reassigned
var counter: i32 = 0;   // mutable
counter += 1;
```

### Functions

Functions are declared with `fn`. The return type comes after the parameter list:

```tether
fn add(x: i32, y: i32) i32 {
    return x + y;
}
```

#### Function Annotations

| Annotation | Effect |
|---|---|
| `pure` | Guarantees no side effects — enables CSE, hoisting, and readonly/readnone LLVM attributes |
| `noalloc` | Guarantees zero heap allocation — critical for HFT hot paths |
| `inline` | Forces inlining at the call site |

```tether
pure fn square(x: i32) i32 {
    return x * x;
}

noalloc fn compute_hash(v: i32) i64 {
    return v cast i64 * 1099511628211;
}

inline fn double(x: i32) i32 {
    return x + x;
}
```

### Types

#### Primitives

| Type | Description |
|---|---|
| `i8`, `i16`, `i32`, `i64` | Signed integers |
| `u8`, `u16`, `u32`, `u64` | Unsigned integers |
| `f32`, `f64` | Floating-point |
| `bool` | Boolean |
| `void` | No return value |
| `usize` | Pointer-sized unsigned integer |

#### Pointers & References

```tether
*T          // raw pointer
&T          // const reference
&mut T      // mutable reference
[]T         // slice (fat pointer: ptr + length)
```

#### Smart Pointers

```tether
Box<T>      // single-owner heap allocation
Rc<T>       // non-atomic reference counting
Arc<T>      // atomic reference counting
```

#### Error Unions

```tether
!i32        // either i32 or an error code
```

### Structs

```tether
struct Vec2 {
    x: f32,
    y: f32,
}

val v = Vec2{ .x = 1.0, .y = 2.0 };
```

#### SoA Structs

The `soa` keyword transforms a struct into a Structure-of-Arrays layout for cache-friendly bulk processing — ideal for ECS and particle systems:

```tether
soa struct Particle {
    x: f64,
    y: f64,
    z: f64,
    vx: f64,
    vy: f64,
    vz: f64,
    mass: f64,
    active: u8,
}
```

#### Alignment

```tether
align(64) struct CacheLine {
    data: u64,
}
```

### Enums

```tether
enum Color {
    Red,
    Green,
    Blue,
}

enum Status {
    Ok = 0,
    Error = 1,
    Timeout = 2,
}
```

### Match (Pattern Matching)

```tether
fn color_code(c: Color) i32 {
    match (c) {
        Color.Red -> {
            return 0xFF0000;
        }
        Color.Green -> {
            return 0x00FF00;
        }
        Color.Blue -> {
            return 0x0000FF;
        }
    }
}
```

### Control Flow

#### If / Else

```tether
if (x > 0) {
    return x;
} else if (x < 0) {
    return -x;
} else {
    return 0;
}
```

#### While Loops

Tether supports an increment clause in `while`, similar to Zig:

```tether
var i: i32 = 0;
while (i < n) : (i += 1) {
    // loop body
}
```

#### Break & Continue

```tether
while (i > 0) : (i -= 1) {
    if (i == 5) { break; }
    if (i % 2 == 0) { continue; }
    count += 1;
}
```

### Error Handling

Tether uses Zig-style error unions with `try` and `errdefer`:

```tether
fn may_fail(input: i32) !i32 {
    if (input < 0) {
        return 0;     // error return
    }
    return input * 2;
}

fn safe_call(x: i32) i32 {
    val result: i32 = try may_fail(x);   // propagates error on failure
    return result;
}

fn error_cleanup(x: i32) !i32 {
    var temp: i32 = x;
    errdefer temp = 0;    // runs only on error path
    if (x < 0) {
        return 0;
    }
    return temp + 1;
}
```

### Defer

Deferred statements execute when the function exits, regardless of how it exits (normal return, early return, or error):

```tether
fn resource_example() i32 {
    var result: i32 = 0;
    defer result = 0;     // runs on function exit
    result = 42;
    return result;        // result is set to 0 before returning
}
```

### Cast

```tether
val x: i32 = 42;
val y: i64 = x cast i64;
```

### Atomic Operations

```tether
atomic(release) counter += 1;
```

### Yield (Cooperative Scheduling)

```tether
while (i < n) : (i += 1) {
    if (i % 100 == 0) {
        yield;    // cooperative yield point
    }
}
```

### Compiler Directives

| Directive | Effect |
|---|---|
| `@simd` | Hint for SIMD vectorization |
| `@polly` | Hint for the Polly polyhedral optimizer |
| `@superoptimize` | Aggressive superoptimization |
| `@tailcall` | Guarantee tail call optimization |
| `@black_box` | Optimization barrier (prevents DCE) |

```tether
@simd
fn vector_sum(data: []f64, len: u64) f64 {
    var total: f64 = 0.0;
    var i: u64 = 0;
    while (i < len) {
        total = total + data[i];
        i += 1;
    }
    return total;
}
```

### Operators

#### Arithmetic
`+  -  *  /  %`

#### Compound Assignment
`+=  -=  *=  /=  %=`

#### Bitwise
`&  |  ^  <<  >>`

#### Comparison
`==  !=  <  >  <=  >=`

#### Logical
`&&  ||  !`

## Compilation Pipeline

Tether compiles through a 10-phase pipeline:

```
.tth source
    │
    ▼
┌─────────────────┐
│  1. Read source  │
└────────┬────────┘
         ▼
┌─────────────────┐
│  2. Lexer        │   Zero-copy string_view tokens, lookup-table optimized
└────────┬────────┘
         ▼
┌─────────────────┐
│  3. Parser       │   Recursive-descent → AST
└────────┬────────┘
         ▼
┌─────────────────┐
│  4. Sema         │   Type checking, symbol resolution, struct layout
└────────┬────────┘
         ▼
┌─────────────────────┐
│  5. Pre-LLVM Opt    │   12 optimization passes (tiered: None/Basic/Aggressive)
└────────┬────────────┘
         ▼
┌─────────────────┐
│  6. CFG Build    │   Control-flow graph construction
└────────┬────────┘
         ▼
┌─────────────────┐
│  7. Borrow Check │   Strict-mode borrow checker with noalias propagation
└────────┬────────┘
         ▼
┌─────────────────┐
│  8. IR Gen       │   SSA codegen with phi nodes, zero-allocas for scalars
└────────┬────────┘
         ▼
┌─────────────────┐
│  9. Write IR     │   Output .ll file
└────────┬────────┘
         ▼
┌─────────────────┐
│ 10. Backend      │   clang/llc → .o/.s, link with runtime + -lm
└─────────────────┘
```

At `-O2+`, the backend uses ThinLTO (`-flto=thin`) for cross-module optimization. The optional TetherAttrPass LLVM plugin can inject Tether-specific optimization attributes (noalias, readonly, hot/cold, memory(none), willreturn) based on metadata.

## Pre-LLVM Optimization Passes

The PreLLVMPipeline runs 12 optimization passes before LLVM even sees the code:

| Pass | Tier | What It Does |
|---|---|---|
| MonomorphizationPass | Basic | Generic monomorphization |
| DeferCoalescer | Basic | Merge consecutive defer chains |
| ErrorPathSeparator | Basic | Mark error paths as cold for branch layout |
| ComptimeEvaluator | Aggressive | Compile-time expression evaluation |
| AoSToSoA | Aggressive | Struct-of-Arrays transformation for `soa struct` |
| FieldReorderer | Aggressive | Cache-optimal struct field ordering |
| SROAPass | Aggressive | Scalar Replacement of Aggregates |
| AllocFusionPass | Aggressive | Batch allocation fusion |
| AllocatorLowerer | Aggressive | Arena bump allocation inline lowering |
| EscapeAnalysis | Aggressive | Identify non-escaping allocations |
| HotColdSplitter | Aggressive | PGO-guided hot/cold field splitting |
| NichedErrorPass | Aggressive | Niched error representation optimization |
| OpaqueBarrier | Aggressive | FFI boundary aliasing prevention |
| PrefetchInserter | Aggressive | Align-guided prefetch insertion |
| YieldPointInserter | Aggressive | Cooperative scheduler yield points |
| SpeculativeOptimizer | Aggressive | Speculative optimization with deoptimization |

Pass tiers map to optimization levels:
- **-O0**: No pre-LLVM passes
- **-O1, -Os, -Oz**: Basic passes only
- **-O2, -O3**: Full aggressive suite

## Profile-Guided Optimization (PGO)

Tether supports PGO for profile-driven optimization:

```bash
# One-command PGO cycle
make pgo SOURCE=benchmarks/propeller_bench.tth

# Or step by step:
tetherc --profile-generate bench.tth -o bench_inst   # instrument
./bench_inst                                          # collect profile
tetherc --profile-use default.profdata bench.tth      # optimize with profile
```

### Propeller (PGO + Code Layout)

For maximum performance, Tether integrates with LLVM's Propeller pipeline (requires LLVM 19+ with lld):

```bash
# Full pipeline: instrument → profile → optimize with BB reordering + lld
make propeller-full INPUT=benchmarks/propeller_bench.tth

# Individual phases:
make propeller-instrument INPUT=benchmarks/propeller_bench.tth
make propeller-profile BINARY=/tmp/propeller_bench_inst
make propeller-optimize INPUT=benchmarks/propeller_bench.tth
```

## Cross-Compilation

```bash
tetherc --target aarch64 hello.tth    # ARM64
tetherc --target riscv64 hello.tth    # RISC-V 64
tetherc --target wasm32 hello.tth     # WebAssembly 32
```

## Runtime Library

The C runtime (`runtime/tether_runtime.h/.c`) provides:

- **Allocators** — Arena, fixed-buffer, and heap allocators (with mimalloc integration)
- **Smart pointers** — `Box<T>`, `Rc<T>`, `Arc<T>`
- **Slices** — Fat pointer `{ ptr, len }`
- **Error results** — `TetherError { value, error_code, is_error }`
- **Task pool** — Work-stealing thread pool with Chase-Lev deques
- **Batch allocation** — Allocation fusion for multiple `Box.new()` calls
- **I/O** — `tether_print_i32`, `tether_print_f64`, `tether_print_str`
- **Benchmarking** — `black_box`, `volatile_read`, `volatile_write`
- **Cooperative yield** — `tether_yield()` for coroutine/GC integration
- **Deoptimization** — `tether_deopt()` with custom handler registration

## Benchmarks

Tether is designed to match or beat C on compute workloads. Current micro-benchmark results (GCC -O3 vs tetherc -O3, 10-iteration median, anti-DCE methodology):

| Benchmark | Tether vs C | Notes |
|---|---|---|
| float_math | **2.0x faster** | LLVM generates excellent float codegen |
| int_div | **1.02x faster** | Beats C by 2% |
| fib_iter | 1.13x slower | Competitive |
| int_arith | 1.62x slower | Extra instructions for power-of-2 signed division |
| branching | 2.68x slower | Select optimization vs predicted branches |

Run benchmarks:

```bash
make bench              # Full suite (compiler throughput + real workload + ECS)
make bench-speed        # Compiler phase throughput
make bench-real         # Real workload compilation
make bench-ecs          # ECS SoA vs AoS runtime
```

## Testing

```bash
make test               # Compiles all examples/*.tth with --emit-ir
```

Crash regression tests are in `crash_tests/` — 28 `.tth` files covering edge cases that previously caused crashes (nested structs, recursive types, empty functions, deep nesting, large enums, etc.).

## Project Structure

```
├── src/                   Compiler source (C++17)
│   ├── driver/            Entry point & pipeline orchestrator
│   ├── lexer/             Lookup-table tokenizer
│   ├── parser/            Recursive-descent parser
│   ├── ast/               AST utilities
│   ├── sema/              Semantic analysis & type checking
│   ├── cfg/               Control-flow graph builder
│   ├── borrowck/          Borrow checker
│   ├── opt/               12 pre-LLVM optimization passes
│   ├── metadata/          Metadata engine (8 components)
│   ├── codegen/           LLVM IR generation (SSA + phi nodes)
│   ├── diag/              Error reporting
│   └── pass/              LLVM pass plugin (TetherAttrPass)
├── include/               Header files (mirrors src/)
├── runtime/               C runtime library
├── examples/              21 example .tth programs
├── crash_tests/           28 crash regression tests
├── tests/                 C++ unit tests
├── benchmarks/            Benchmark suite
│   ├── micro/             Micro-benchmarks
│   ├── real_workload/     Real workload programs
│   ├── ecs/               ECS SoA vs AoS
│   ├── heavy/             Heavy computation
│   └── techempower/       TechEmpower-style web benchmarks
├── scripts/               Build scripts (PGO, Propeller)
├── Makefile               Build system
└── Makefile.asan          AddressSanitizer build variant
```

## Build Targets

| Target | Description |
|---|---|
| `all` | Build compiler + runtime (default) |
| `clean` | Remove build artifacts |
| `test` | Compile all examples |
| `bench` | Run full benchmark suite |
| `bench-speed` | Compiler phase throughput |
| `bench-real` | Real workload compilation |
| `bench-ecs` | ECS runtime benchmark |
| `pass-plugin` | Build TetherAttrPass LLVM plugin |
| `install` | Install to PREFIX (default: /usr/local) |
| `uninstall` | Remove installed files |
| `pgo` | One-command PGO cycle |
| `propeller-full` | Full PGO + Propeller pipeline |
| `setup` | Configure git hooks |
| `help` | Show all targets |

## AddressSanitizer Build

For debugging memory issues, use the ASan build variant:

```bash
make -f Makefile.asan
```

This builds with `-O0 -g -fsanitize=address -fno-omit-frame-pointer`.

## License

GNU Affero General Public License v3.0 — see [LICENSE](LICENSE) for details.
