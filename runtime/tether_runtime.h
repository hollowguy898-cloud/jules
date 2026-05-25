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
    void*    ptr;      /* Heap-allocated data */
    int64_t  size;     /* Size of the allocation in bytes */
} TetherBox;

/* ============================================================================
 * Rc<T> - reference-counted pointer (non-atomic)
 *
 * Layout:  { i64 refcount, T data }
 * The Rc struct holds a pointer to this layout.  When the refcount drops to
 * zero, the data is freed.
 * ============================================================================ */

typedef struct TetherRc {
    void*    ptr;      /* Points to: { int64_t refcount; char data[]; } */
    int64_t  data_size; /* Size of the data portion in bytes */
} TetherRc;

/* ============================================================================
 * Arc<T> - atomically reference-counted pointer
 *
 * Layout:  { _Atomic int64_t refcount, T data }
 * Same as Rc but uses atomic operations for the refcount.
 * ============================================================================ */

typedef struct TetherArc {
    void*    ptr;      /* Points to: { _Atomic int64_t refcount; char data[]; } */
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
} TetherArena;

/* Initialize an arena with the given buffer and capacity */
void tether_arena_init(TetherArena* arena, void* buffer, int64_t capacity);

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
 * Box operations
 * ============================================================================ */

/* Create a new Box by copying `size` bytes from `data` */
TetherBox tether_box_new(const void* data, int64_t size);

/* Free a Box */
void tether_box_drop(TetherBox* box);

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
int64_t tether_rc_count(TetherRc* rc);

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
int64_t tether_arc_count(TetherArc* arc);

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

/* Print a newline */
void tether_print_ln(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TETHER_RUNTIME_H */
