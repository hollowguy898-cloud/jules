/**
 * jules_runtime.h - Runtime support library for the Jules programming language
 *
 * This header provides:
 *   - Allocator interface (function pointer table)
 *   - JulesSlice<T> (generic slice: pointer + length)
 *   - Box<T>, Rc<T>, Arc<T> smart pointer structs
 *   - JulesError result struct
 *   - Arena allocator
 *   - Fixed-buffer allocator
 *   - Heap allocator (wraps malloc/free/realloc)
 *   - Print functions for primitive types
 *
 * All functions are declared with extern "C" linkage for LLVM IR compatibility.
 */

#ifndef JULES_RUNTIME_H
#define JULES_RUNTIME_H

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

typedef struct JulesAllocator JulesAllocator;

/* Function pointer types for the allocator vtable */
typedef void* (*JulesAllocFn)(JulesAllocator* alloc, size_t size);
typedef void* (*JulesReallocFn)(JulesAllocator* alloc, void* ptr, size_t old_size, size_t new_size);
typedef void  (*JulesFreeFn)(JulesAllocator* alloc, void* ptr, size_t size);
typedef void  (*JulesResetFn)(JulesAllocator* alloc);

struct JulesAllocator {
    void*           ctx;      /* Opaque context / state pointer */
    JulesAllocFn    alloc;    /* Allocate `size` bytes */
    JulesReallocFn  realloc;  /* Reallocate from old_size to new_size */
    JulesFreeFn     free;     /* Free a previous allocation */
    JulesResetFn    reset;    /* Reset the allocator (free all at once) */
};

/* ============================================================================
 * JulesSlice - a fat pointer representing a view into a contiguous sequence
 *
 * In LLVM IR, a slice is represented as { T*, i64 } — a pointer and a length.
 * In C, we use macros to instantiate the struct for each element type.
 * ============================================================================ */

#define JULES_SLICE(T) \
    struct JulesSlice_##T { \
        T*     ptr;  \
        int64_t len;  \
    }

/* Generic slice using void* for type-erased contexts */
typedef struct JulesSlice_void {
    void*    ptr;
    int64_t  len;
} JulesSlice_void;

/* Common slice types */
JULES_SLICE(uint8_t);   /* JulesSlice_uint8_t */
JULES_SLICE(int32_t);   /* JulesSlice_int32_t */
JULES_SLICE(int64_t);   /* JulesSlice_int64_t */
JULES_SLICE(float);     /* JulesSlice_float   */
JULES_SLICE(double);    /* JulesSlice_double  */

/* ============================================================================
 * Box<T> - single-owner heap allocation
 *
 * Box is represented as a simple pointer.  When the Box goes out of scope,
 * the drop function is called and the memory is freed.
 * In the runtime, we track a Box as a void* plus a size for deallocation.
 * ============================================================================ */

typedef struct JulesBox {
    void*    ptr;      /* Heap-allocated data */
    int64_t  size;     /* Size of the allocation in bytes */
} JulesBox;

/* ============================================================================
 * Rc<T> - reference-counted pointer (non-atomic)
 *
 * Layout:  { i64 refcount, T data }
 * The Rc struct holds a pointer to this layout.  When the refcount drops to
 * zero, the data is freed.
 * ============================================================================ */

typedef struct JulesRc {
    void*    ptr;      /* Points to: { int64_t refcount; char data[]; } */
    int64_t  data_size; /* Size of the data portion in bytes */
} JulesRc;

/* ============================================================================
 * Arc<T> - atomically reference-counted pointer
 *
 * Layout:  { _Atomic int64_t refcount, T data }
 * Same as Rc but uses atomic operations for the refcount.
 * ============================================================================ */

typedef struct JulesArc {
    void*    ptr;      /* Points to: { _Atomic int64_t refcount; char data[]; } */
    int64_t  data_size; /* Size of the data portion in bytes */
} JulesArc;

/* ============================================================================
 * JulesError - error result type
 *
 * When a jules function returns T!E, the LLVM IR returns a struct:
 *   { T value, i1 error_flag }
 * The runtime provides a generic container for this.
 * ============================================================================ */

typedef struct JulesError {
    void*    value;      /* Pointer to the success value (stack or heap) */
    int32_t  error_code; /* 0 = success, non-zero = error */
    bool     is_error;   /* True if the result is an error */
} JulesError;

/* ============================================================================
 * Arena allocator
 *
 * A simple bump allocator that allocates from a fixed buffer.
 * Free is a no-op; reset frees everything at once.
 * ============================================================================ */

typedef struct JulesArena {
    char*    buffer;       /* Start of the arena buffer */
    int64_t  capacity;     /* Total buffer size in bytes */
    int64_t  offset;       /* Current bump offset */
} JulesArena;

/* Initialize an arena with the given buffer and capacity */
void jules_arena_init(JulesArena* arena, void* buffer, int64_t capacity);

/* Allocate `size` bytes from the arena. Returns NULL if exhausted. */
void* jules_arena_alloc(JulesArena* arena, int64_t size);

/* Free is a no-op for arena allocation (individual frees are ignored) */
void jules_arena_free(JulesArena* arena, void* ptr, int64_t size);

/* Reset the arena, freeing all allocations at once */
void jules_arena_reset(JulesArena* arena);

/* Create a JulesAllocator vtable wrapping an arena */
JulesAllocator jules_arena_allocator(JulesArena* arena);

/* ============================================================================
 * Fixed-buffer allocator
 *
 * Allocates from a fixed buffer. Returns NULL if the buffer is exhausted.
 * No individual frees are supported.
 * ============================================================================ */

typedef struct JulesFixedBuffer {
    char*    buffer;       /* Start of the buffer */
    int64_t  capacity;     /* Total buffer size in bytes */
    int64_t  offset;       /* Current allocation offset */
} JulesFixedBuffer;

/* Initialize a fixed-buffer allocator */
void jules_fixed_buffer_init(JulesFixedBuffer* fb, void* buffer, int64_t capacity);

/* Allocate `size` bytes. Returns NULL if exhausted. */
void* jules_fixed_buffer_alloc(JulesFixedBuffer* fb, int64_t size);

/* Free is a no-op */
void jules_fixed_buffer_free(JulesFixedBuffer* fb, void* ptr, int64_t size);

/* Reset the fixed buffer (allow reusing the buffer) */
void jules_fixed_buffer_reset(JulesFixedBuffer* fb);

/* Create a JulesAllocator vtable wrapping a fixed buffer */
JulesAllocator jules_fixed_buffer_allocator(JulesFixedBuffer* fb);

/* ============================================================================
 * Heap allocator
 *
 * Thin wrappers around malloc/free/realloc.
 * ============================================================================ */

/* Allocate `size` bytes on the heap */
void* jules_heap_alloc(JulesAllocator* alloc, size_t size);

/* Reallocate from old_size to new_size */
void* jules_heap_realloc(JulesAllocator* alloc, void* ptr, size_t old_size, size_t new_size);

/* Free a heap allocation */
void jules_heap_free(JulesAllocator* alloc, void* ptr, size_t size);

/* Reset is a no-op for heap allocator */
void jules_heap_reset(JulesAllocator* alloc);

/* Create a JulesAllocator vtable wrapping the heap */
JulesAllocator jules_heap_allocator(void);

/* ============================================================================
 * Box operations
 * ============================================================================ */

/* Create a new Box by copying `size` bytes from `data` */
JulesBox jules_box_new(const void* data, int64_t size);

/* Free a Box */
void jules_box_drop(JulesBox* box);

/* Get a pointer to the Box's data */
void* jules_box_deref(JulesBox* box);

/* ============================================================================
 * Rc operations (non-atomic reference counting)
 * ============================================================================ */

/* Create a new Rc by copying `size` bytes from `data` */
JulesRc jules_rc_new(const void* data, int64_t size);

/* Clone an Rc (increments refcount) */
JulesRc jules_rc_clone(JulesRc* rc);

/* Drop an Rc (decrements refcount; frees if zero) */
void jules_rc_drop(JulesRc* rc);

/* Get a pointer to the Rc's data */
void* jules_rc_deref(JulesRc* rc);

/* Get the current reference count (for debugging) */
int64_t jules_rc_count(JulesRc* rc);

/* ============================================================================
 * Arc operations (atomic reference counting)
 * ============================================================================ */

/* Create a new Arc by copying `size` bytes from `data` */
JulesArc jules_arc_new(const void* data, int64_t size);

/* Clone an Arc (atomically increments refcount) */
JulesArc jules_arc_clone(JulesArc* arc);

/* Drop an Arc (atomically decrements refcount; frees if zero) */
void jules_arc_drop(JulesArc* arc);

/* Get a pointer to the Arc's data */
void* jules_arc_deref(JulesArc* arc);

/* Get the current reference count (for debugging) */
int64_t jules_arc_count(JulesArc* arc);

/* ============================================================================
 * Print functions
 *
 * These are called by the jules codegen to implement print intrinsics.
 * All use printf under the hood.
 * ============================================================================ */

/* Print a 32-bit signed integer followed by nothing (no newline) */
void jules_print_i32(int32_t value);

/* Print a 64-bit signed integer followed by nothing */
void jules_print_i64(int64_t value);

/* Print a 32-bit floating-point number followed by nothing */
void jules_print_f32(float value);

/* Print a 64-bit floating-point number followed by nothing */
void jules_print_f64(double value);

/* Print a boolean ("true" or "false") followed by nothing */
void jules_print_bool(bool value);

/* Print a UTF-8 string with given length followed by nothing */
void jules_print_str(const char* str, int64_t len);

/* Print a newline */
void jules_print_ln(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* JULES_RUNTIME_H */
