/**
 * tether_runtime.h - Runtime support library for the Tether programming language
 *
 * This header provides:
 *   - Allocator interface (function pointer table)
 *   - TetherSlice<T> (generic slice: pointer + length)
 *   - Box<T>, Rc<T>, Arc<T> smart pointer structs
 *   - TetherError result struct
 *   - Arena allocator
 *   - Fixed-buffer allocator
 *   - Heap allocator (wraps malloc/free/realloc)
 *   - Print functions for primitive types
 *
 * All functions are declared with extern "C" linkage for LLVM IR compatibility.
 */

#ifndef TETHER_RUNTIME_H
#define TETHER_RUNTIME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Allocator abstraction — mimalloc / malloc dispatch
 *
 * By default, Tether uses mimalloc for 2-3x faster allocation on hot paths.
 * If mimalloc is not available (TETHER_NO_MIMALLOC defined), falls back to
 * system malloc. The mimalloc library is preferred because:
 *   - 2-3x faster than glibc malloc for small allocations (Box, Rc, Arc)
 *   - Better multithreaded scalability (thread-local segments)
 *   - Lower memory fragmentation
 *   - Consistent low-latency allocation (important for HFT workloads)
 *
 * To force system malloc: compile with -DTETHER_NO_MIMALLOC
 * ============================================================================ */

#ifdef TETHER_NO_MIMALLOC
/* Explicitly disabled — use system malloc */
#define tether_malloc(size)        malloc(size)
#define tether_free(ptr)           free(ptr)
#define tether_realloc(ptr, size)  realloc(ptr, size)
#elif defined(TETHER_USE_MIMALLOC)
/* Explicitly enabled via flag */
#include <mimalloc.h>
#define tether_malloc(size)        mi_malloc(size)
#define tether_free(ptr)           mi_free(ptr)
#define tether_realloc(ptr, size)  mi_realloc(ptr, size)
#else
/* Default: try mimalloc, fall back to malloc if not linked */
#if __has_include(<mimalloc.h>)
#include <mimalloc.h>
#define tether_malloc(size)        mi_malloc(size)
#define tether_free(ptr)           mi_free(ptr)
#define tether_realloc(ptr, size)  mi_realloc(ptr, size)
#else
#define tether_malloc(size)        malloc(size)
#define tether_free(ptr)           free(ptr)
#define tether_realloc(ptr, size)  realloc(ptr, size)
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Allocator interface
 *
 * The Allocator struct is a function-pointer table that allows jules programs
 * to use custom allocation strategies.  Each function takes a `ctx` pointer
 * that is the allocator's internal state.
 * ============================================================================ */

typedef struct TetherAllocator TetherAllocator;

/* Function pointer types for the allocator vtable */
typedef void* (*JulesAllocFn)(TetherAllocator* alloc, size_t size);
typedef void* (*JulesReallocFn)(TetherAllocator* alloc, void* ptr, size_t old_size, size_t new_size);
typedef void  (*JulesFreeFn)(TetherAllocator* alloc, void* ptr, size_t size);
typedef void  (*JulesResetFn)(TetherAllocator* alloc);

struct TetherAllocator {
    void*           ctx;      /* Opaque context / state pointer */
    JulesAllocFn    alloc;    /* Allocate `size` bytes */
    JulesReallocFn  realloc;  /* Reallocate from old_size to new_size */
    JulesFreeFn     free;     /* Free a previous allocation */
    JulesResetFn    reset;    /* Reset the allocator (free all at once) */
};

/* ============================================================================
 * TetherSlice - a fat pointer representing a view into a contiguous sequence
 *
 * In LLVM IR, a slice is represented as { T*, i64 } — a pointer and a length.
 * In C, we use macros to instantiate the struct for each element type.
 * ============================================================================ */

#define TETHER_SLICE(T) \
    struct TetherSlice_##T { \
        T*     ptr;  \
        int64_t len;  \
    }

/* Generic slice using void* for type-erased contexts */
typedef struct TetherSlice_void {
    void*    ptr;
    int64_t  len;
} TetherSlice_void;

/* Common slice types */
TETHER_SLICE(uint8_t);   /* TetherSlice_uint8_t */
TETHER_SLICE(int32_t);   /* TetherSlice_int32_t */
TETHER_SLICE(int64_t);   /* TetherSlice_int64_t */
TETHER_SLICE(float);     /* TetherSlice_float   */
TETHER_SLICE(double);    /* TetherSlice_double  */

/* ============================================================================
 * Box<T> - single-owner heap allocation
 *
 * Box is represented as a simple pointer.  When the Box goes out of scope,
 * the drop function is called and the memory is freed.
 * In the runtime, we track a Box as a void* plus a size for deallocation.
 * ============================================================================ */

typedef struct TetherBox {
    void*    ptr;      /* Heap-allocated data pointer (like Rust's Box — size known at compile time) */
} TetherBox;

/* ============================================================================
 * Rc<T> - reference-counted pointer (non-atomic)
 *
 * Layout:  { i32 refcount, T data }
 * The Rc struct holds a pointer to this layout.  When the refcount drops to
 * zero, the data is freed.
 * ============================================================================ */

typedef struct TetherRc {
    void*    ptr;      /* Points to: { int32_t refcount; char data[]; } */
    int64_t  data_size; /* Size of the data portion in bytes */
} TetherRc;

/* ============================================================================
 * Arc<T> - atomically reference-counted pointer
 *
 * Layout:  { _Atomic int32_t refcount, T data }
 * Same as Rc but uses atomic operations for the refcount.
 * ============================================================================ */

typedef struct TetherArc {
    void*    ptr;      /* Points to: { _Atomic int32_t refcount; char data[]; } */
    int64_t  data_size; /* Size of the data portion in bytes */
} TetherArc;

/* ============================================================================
 * TetherError - error result type
 *
 * When a jules function returns T!E, the LLVM IR returns a struct:
 *   { T value, i1 error_flag }
 * The runtime provides a generic container for this.
 * ============================================================================ */

typedef struct TetherError {
    void*    value;      /* Pointer to the success value (stack or heap) */
    int32_t  error_code; /* 0 = success, non-zero = error */
    bool     is_error;   /* True if the result is an error */
} TetherError;

/* ============================================================================
 * Arena allocator
 *
 * A simple bump allocator that allocates from a fixed buffer.
 * Free is a no-op; reset frees everything at once.
 * ============================================================================ */

typedef struct TetherArena {
    char*    buffer;       /* Start of the arena buffer */
    int64_t  capacity;     /* Total buffer size in bytes */
    int64_t  offset;       /* Current bump offset */
    int64_t  alignment;    /* Alignment for allocations (default: 16, was 8) */
} TetherArena;

/* Initialize an arena with the given buffer, capacity, and alignment */
void tether_arena_init(TetherArena* arena, void* buffer, int64_t capacity, int64_t alignment);

/* Convenience macro: init arena with default 16-byte alignment */
#define tether_arena_init_default(arena, buffer, capacity) \
    tether_arena_init((arena), (buffer), (capacity), 16)

/* Allocate `size` bytes from the arena. Returns NULL if exhausted. */
void* tether_arena_alloc(TetherArena* arena, int64_t size);

/* Free is a no-op for arena allocation (individual frees are ignored) */
void tether_arena_free(TetherArena* arena, void* ptr, int64_t size);

/* Reset the arena, freeing all allocations at once */
void tether_arena_reset(TetherArena* arena);

/* Create a TetherAllocator vtable wrapping an arena */
TetherAllocator tether_arena_allocator(TetherArena* arena);

/* ============================================================================
 * Fixed-buffer allocator
 *
 * Allocates from a fixed buffer. Returns NULL if the buffer is exhausted.
 * No individual frees are supported.
 * ============================================================================ */

typedef struct TetherFixedBuffer {
    char*    buffer;       /* Start of the buffer */
    int64_t  capacity;     /* Total buffer size in bytes */
    int64_t  offset;       /* Current allocation offset */
} TetherFixedBuffer;

/* Initialize a fixed-buffer allocator */
void tether_fixed_buffer_init(TetherFixedBuffer* fb, void* buffer, int64_t capacity);

/* Allocate `size` bytes. Returns NULL if exhausted. */
void* tether_fixed_buffer_alloc(TetherFixedBuffer* fb, int64_t size);

/* Free is a no-op */
void tether_fixed_buffer_free(TetherFixedBuffer* fb, void* ptr, int64_t size);

/* Reset the fixed buffer (allow reusing the buffer) */
void tether_fixed_buffer_reset(TetherFixedBuffer* fb);

/* Create a TetherAllocator vtable wrapping a fixed buffer */
TetherAllocator tether_fixed_buffer_allocator(TetherFixedBuffer* fb);

/* ============================================================================
 * Heap allocator
 *
 * Thin wrappers around malloc/free/realloc.
 * ============================================================================ */

/* Allocate `size` bytes on the heap */
void* tether_heap_alloc(TetherAllocator* alloc, size_t size);

/* Reallocate from old_size to new_size */
void* tether_heap_realloc(TetherAllocator* alloc, void* ptr, size_t old_size, size_t new_size);

/* Free a heap allocation */
void tether_heap_free(TetherAllocator* alloc, void* ptr, size_t size);

/* Reset is a no-op for heap allocator */
void tether_heap_reset(TetherAllocator* alloc);

/* Create a TetherAllocator vtable wrapping the heap */
TetherAllocator tether_heap_allocator(void);

/* ============================================================================
 * Thread-local small-box cache
 *
 * Avoids the global allocator lock for common small allocations (≤256 bytes).
 * Each thread gets its own cache of pre-allocated blocks.
 * ============================================================================ */

#define TETHER_SMALL_BOX_CACHE_SIZE 64
#define TETHER_SMALL_BOX_THRESHOLD  256

typedef struct TetherBoxCache {
    void*    blocks[TETHER_SMALL_BOX_CACHE_SIZE];
    int      count;
    int      capacity;
} TetherBoxCache;

/* Get the thread-local Box cache */
TetherBoxCache* tether_box_cache_get(void);

/* Allocate from the thread-local cache, or fall through to allocator */
void* tether_box_cache_alloc(TetherBoxCache* cache, int64_t size);

/* Return a block to the thread-local cache */
void tether_box_cache_free(TetherBoxCache* cache, void* ptr, int64_t size);

/* ============================================================================
 * Batched Allocation — Allocation Fusion
 *
 * Multiple Box.new() calls in sequence can be fused into a single allocation.
 * The caller allocates a batch, then carves individual Boxes out of it.
 * Similar to Zig's ArenaAllocator but at the type level.
 * ============================================================================ */

typedef struct TetherAllocBatch {
    void*    buffer;     /* Pre-allocated memory chunk */
    int64_t  capacity;   /* Total buffer size */
    int64_t  offset;     /* Current carve offset */
    int64_t  count;      /* Number of allocations carved from this batch */
} TetherAllocBatch;

/* Create a batch allocator with the given total size */
TetherAllocBatch tether_batch_alloc(int64_t total_size);

/* Carve `size` bytes from the batch. Returns NULL if exhausted. */
void* tether_batch_carve(TetherAllocBatch* batch, int64_t size);

/* Free the entire batch at once (all individual allocations become invalid) */
void tether_batch_free(TetherAllocBatch* batch);

/* ============================================================================
 * Box operations
 * ============================================================================ */

/* Create a new Box by copying `size` bytes from `data` (backward compat, default size=0) */
TetherBox tether_box_new(const void* data, int64_t size);

/* Free a Box (backward compat, uses default size=0 for cache) */
void tether_box_drop(TetherBox* box);

/* Create a new Box with explicit size (IR generator emits size as immediate) */
TetherBox tether_box_new_sized(const void* data, int64_t size);

/* Free a Box with explicit size (IR generator emits size as immediate to drop) */
void tether_box_drop_sized(TetherBox* box, int64_t size);

/* Get a pointer to the Box's data */
void* tether_box_deref(TetherBox* box);

/* ============================================================================
 * Rc operations (non-atomic reference counting)
 * ============================================================================ */

/* Create a new Rc by copying `size` bytes from `data` */
TetherRc tether_rc_new(const void* data, int64_t size);

/* Clone an Rc (increments refcount) */
TetherRc tether_rc_clone(TetherRc* rc);

/* Drop an Rc (decrements refcount; frees if zero) */
void tether_rc_drop(TetherRc* rc);

/* Get a pointer to the Rc's data */
void* tether_rc_deref(TetherRc* rc);

/* Get the current reference count (for debugging) */
int32_t tether_rc_count(TetherRc* rc);

/* ============================================================================
 * Arc operations (atomic reference counting)
 * ============================================================================ */

/* Create a new Arc by copying `size` bytes from `data` */
TetherArc tether_arc_new(const void* data, int64_t size);

/* Clone an Arc (atomically increments refcount) */
TetherArc tether_arc_clone(TetherArc* arc);

/* Drop an Arc (atomically decrements refcount; frees if zero) */
void tether_arc_drop(TetherArc* arc);

/* Get a pointer to the Arc's data */
void* tether_arc_deref(TetherArc* arc);

/* Get the current reference count (for debugging) */
int32_t tether_arc_count(TetherArc* arc);

/* ============================================================================
 * Print functions
 *
 * These are called by the jules codegen to implement print intrinsics.
 * All use printf under the hood.
 * ============================================================================ */

/* Print a 32-bit signed integer followed by nothing (no newline) */
void tether_print_i32(int32_t value);

/* Print a 64-bit signed integer followed by nothing */
void tether_print_i64(int64_t value);

/* Print a 32-bit floating-point number followed by nothing */
void tether_print_f32(float value);

/* Print a 64-bit floating-point number followed by nothing */
void tether_print_f64(double value);

/* Print a boolean ("true" or "false") followed by nothing */
void tether_print_bool(bool value);

/* Print a UTF-8 string with given length followed by nothing */
void tether_print_str(const char* str, int64_t len);

/* ============================================================================
 * Optimized string operations
 *
 * Fast string equality and zero-copy string slicing. These are used by
 * the Tether codegen to optimize string comparisons and substring operations.
 * ============================================================================ */

/* Fast string equality — checks length first, then pointer equality (for
 * interned strings), then falls back to memcmp.
 * Returns true if the strings are equal, false otherwise.
 * When the Tether compiler interns string literals, identical strings
 * share the same pointer, making this O(1) in the common case. */
bool tether_str_eq(const char* a, int64_t a_len, const char* b, int64_t b_len);

/* Zero-copy string slice — just adjusts pointer and length.
 * No memcpy, no allocation. The returned slice points into the
 * original string's memory, making substring operations O(1). */
TetherSlice_void tether_str_slice(const char* str, int64_t str_len,
                                   int64_t start, int64_t len);

/* Print a newline */
void tether_print_ln(void);

// ============================================================================
// Spawn / Task Pool — Work-stealing thread pool for structured async
//
// spawn expr; schedules a function call onto a lock-free work-stealing
// thread pool. The spawned task returns a TetherTaskHandle that can be
// awaited for the result.
// ============================================================================

// Opaque task handle — represents a pending or completed async task
typedef struct TetherTask TetherTask;

// Task function signature: takes a void* context, returns a void* result
typedef void* (*TetherTaskFn)(void* ctx);

// Task handle — wraps a TetherTask pointer for user code
typedef struct TetherTaskHandle {
    TetherTask* task;
} TetherTaskHandle;

// Initialize the global task pool with the given number of worker threads.
// If num_threads is 0, uses the number of hardware threads available.
// Must be called before any spawn() calls. Safe to call multiple times.
void tether_taskpool_init(uint32_t num_threads);

// Shut down the global task pool, waiting for all pending tasks to complete.
// After shutdown, no new tasks can be spawned.
void tether_taskpool_shutdown(void);

// Spawn a task onto the global task pool.
// Returns a handle that can be awaited with tether_task_await().
TetherTaskHandle tether_spawn(TetherTaskFn fn, void* ctx, uint64_t ctx_size);

// Await a spawned task, blocking until it completes.
// Returns the result pointer (or NULL if the task failed).
// After await, the task handle is consumed and must not be used again.
void* tether_task_await(TetherTaskHandle handle);

// Check if a spawned task has completed without blocking.
// Returns 1 if completed, 0 if still running.
int tether_task_is_done(TetherTaskHandle handle);

// Get the number of worker threads in the task pool.
uint32_t tether_taskpool_thread_count(void);

// Configure the adaptive spin parameters for tether_task_await().
// spin_count: number of pure CPU spin iterations (default: 100)
// yield_count: number of sched_yield iterations (default: 1000)
// After both phases, falls through to blocking wait on cond var.
// For HFT workloads: set spin_count high (10000+) and yield_count to 0
// For general workloads: use defaults (100 spin + 1000 yield + block)
// For power-saving: set both to 0 to immediately block
void tether_taskpool_set_spin_params(uint32_t spin_count, uint32_t yield_count);

/* ============================================================================
 * Deoptimization support (Nuclear #8: Speculative Optimization)
 *
 * When the compiler speculatively optimizes code (e.g., assuming a branch is
 * never taken, a pointer is never null, or an array index is in bounds),
 * it inserts deoptimization guards. If a guard fails at runtime, the
 * tether_deopt() function is called to handle the deoptimization event.
 *
 * The default handler prints an error message and aborts. Applications can
 * register a custom handler for more graceful recovery (e.g., JIT compilers
 * can use this for on-stack replacement).
 * ============================================================================ */

/* Deoptimization event information */
typedef struct TetherDeoptInfo {
    uint64_t deopt_id;     /* Which deopt point was hit (maps to source location) */
    void*     frame;       /* Stack frame pointer at the deopt point */
    const char* reason;    /* Human-readable reason for the deopt */
} TetherDeoptInfo;

/* Called when a speculative assumption is violated at runtime.
 * The default implementation prints a message and calls abort().
 * If a custom handler is registered, it is called instead. */
void tether_deopt(uint64_t deopt_id, void* frame);

/* Register a custom deoptimization handler callback.
 * Pass NULL to restore the default handler. */
typedef void (*TetherDeoptCallback)(TetherDeoptInfo* info);
void tether_register_deopt_handler(TetherDeoptCallback callback);

/* ============================================================================
 * SIMD Vector Intrinsics
 *
 * These map directly to LLVM vector instructions for maximum performance.
 * The IRGenerator emits these as LLVM vector operations, not function calls.
 *
 * Type syntax:  simd<T, N>   where T is a primitive type, N is power-of-2
 * Shorthand:    f32x4, f64x2, i32x4, i64x2, i8x16, i16x8,
 *               u8x16, u16x8, u32x4, u64x2
 *
 * The following operations are emitted as LLVM IR vector intrinsics by the
 * code generator — they are NOT C function calls:
 * ============================================================================ */

/* SIMD load/store — aligned for best performance */
/* simd<T, N> simd_load<T, N>(T* ptr) */
/* void simd_store<T, N>(simd<T, N> vec, T* ptr) */

/* SIMD arithmetic — element-wise operations */
/* simd<T, N> simd_add<T, N>(simd<T, N> a, simd<T, N> b) */
/* simd<T, N> simd_sub<T, N>(simd<T, N> a, simd<T, N> b) */
/* simd<T, N> simd_mul<T, N>(simd<T, N> a, simd<T, N> b) */
/* simd<T, N> simd_div<T, N>(simd<T, N> a, simd<T, N> b) */

/* SIMD horizontal reduction */
/* T simd_reduce_add<T, N>(simd<T, N> vec) */
/* T simd_reduce_mul<T, N>(simd<T, N> vec) */
/* T simd_reduce_min<T, N>(simd<T, N> vec) */
/* T simd_reduce_max<T, N>(simd<T, N> vec) */

/* SIMD shuffle/gather/scatter */
/* simd<T, M> simd_shuffle<M, N, T>(simd<T, N> a, simd<T, N> b, int32_t mask[M]) */
/* simd<T, N> simd_gather<T, N>(simd<T*, N> ptrs) */
/* void simd_scatter<T, N>(simd<T, N> vals, simd<T*, N> ptrs) */

/* ============================================================================
 * Benchmarking intrinsics
 *
 * black_box / volatile_barrier — prevent the optimizer from eliminating
 * benchmark code. These are essential for accurate microbenchmarks.
 *
 * Usage in Tether code:
 *   val result = heavy_computation();
 *   @black_box(result);  // Prevents result from being eliminated
 *   // Or simply:
 *   black_box(result);
 * ============================================================================ */

/* black_box — consume a value and return it, but prevent the optimizer from
 * eliminating or simplifying any computation that produced it.
 *
 * This is implemented as a volatile asm block with no outputs, which acts
 * as an optimization barrier. The value is "used" (stored to memory via
 * the asm constraint) but never actually read back, so there's no runtime
 * cost beyond the barrier itself.
 *
 * In LLVM IR, the codegen emits:
 *   call void @llvm.blackbox.i64(i64 %val)  [for integers]
 *   call void @llvm.blackbox.f64(double %val) [for floats]
 *   call void @llvm.blackbox.ptr(ptr %val)    [for pointers]
 */
void tether_black_box_i64(int64_t value);
void tether_black_box_f64(double value);
void tether_black_box_ptr(const void* ptr);

/* volatile_read — read a value from memory with volatile semantics.
 * Forces the compiler to actually load from memory (no CSE, no hoisting).
 * Useful for benchmarking code that reads from shared state.
 */
int64_t tether_volatile_read_i64(const volatile int64_t* ptr);
double tether_volatile_read_f64(const volatile double* ptr);

/* volatile_write — write a value to memory with volatile semantics.
 * Forces the compiler to actually store to memory (no DSE, no sinking).
 */
void tether_volatile_write_i64(volatile int64_t* ptr, int64_t value);
void tether_volatile_write_f64(volatile double* ptr, double value);

/* ============================================================================
 * Array/Slice runtime helpers
 *
 * These functions support array creation, slicing, and iteration in Tether.
 * They are called by the code generator for array literal expressions,
 * for-range loops, and slice operations.
 * ============================================================================ */

/* tether_array_new — allocate a new heap array with zero-initialized elements.
 * Returns a { ptr, i64 } struct (data pointer and length).
 * The returned memory is zero-filled. For small arrays (< 4096 bytes),
 * consider stack allocation instead (the codegen handles this automatically). */
TetherSlice_void tether_array_new(int64_t element_size, int64_t count);

/* tether_array_new_filled — allocate a new heap array, filling all elements
 * with a copy of the given initial value. Returns { ptr, i64 }.
 * This is used for patterns like: val arr = [0; 1024]  (1024 zeros). */
TetherSlice_void tether_array_new_filled(int64_t element_size, int64_t count,
                                           const void* init_value);

/* tether_slice_subslice — create a zero-copy subslice from a slice.
 * Equivalent to Tether's arr[start..end] syntax.
 * No allocation; just adjusts pointer and length. */
TetherSlice_void tether_slice_subslice(const TetherSlice_void* src,
                                         int64_t start, int64_t end);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TETHER_RUNTIME_H */
