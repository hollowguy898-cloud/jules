/**
 * tether_runtime.c - Runtime implementation for the Tether programming language
 *
 * Implements all the runtime functions declared in tether_runtime.h:
 *   - Arena allocator
 *   - Fixed-buffer allocator
 *   - Heap allocator
 *   - Box operations
 *   - Rc operations (non-atomic reference counting)
 *   - Arc operations (atomic reference counting)
 *   - Print functions
 */

#include "tether_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <assert.h>

/* ============================================================================
 * Helper: align up to 8-byte boundary (for arena/buffer allocators)
 * ============================================================================ */

static int64_t align_up(int64_t value, int64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

/* ============================================================================
 * Arena allocator implementation
 * ============================================================================ */

void tether_arena_init(TetherArena* arena, void* buffer, int64_t capacity) {
    arena->buffer   = (char*)buffer;
    arena->capacity = capacity;
    arena->offset   = 0;
}

void* tether_arena_alloc(TetherArena* arena, int64_t size) {
    if (!arena || !arena->buffer || size <= 0) return NULL;

    /* Align the offset to 8 bytes */
    int64_t aligned_offset = align_up(arena->offset, 8);

    if (aligned_offset + size > arena->capacity) {
        return NULL;  /* Arena exhausted */
    }

    void* result = arena->buffer + aligned_offset;
    arena->offset = aligned_offset + size;
    return result;
}

void tether_arena_free(TetherArena* arena, void* ptr, int64_t size) {
    /* Individual frees are no-ops for arena allocation */
    (void)arena;
    (void)ptr;
    (void)size;
}

void tether_arena_reset(TetherArena* arena) {
    if (arena) {
        arena->offset = 0;
    }
}

/* Arena vtable functions */
static void* tether_arena_alloc_fn(TetherAllocator* alloc, size_t size) {
    return tether_arena_alloc((TetherArena*)alloc->ctx, (int64_t)size);
}

static void* tether_arena_realloc_fn(TetherAllocator* alloc, void* ptr,
                                     size_t old_size, size_t new_size) {
    /* Arena doesn't support individual realloc; allocate new and copy */
    (void)old_size;
    void* new_ptr = tether_arena_alloc((TetherArena*)alloc->ctx, (int64_t)new_size);
    if (new_ptr && ptr) {
        memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    }
    return new_ptr;
}

static void tether_arena_free_fn(TetherAllocator* alloc, void* ptr, size_t size) {
    tether_arena_free((TetherArena*)alloc->ctx, ptr, (int64_t)size);
}

static void tether_arena_reset_fn(TetherAllocator* alloc) {
    tether_arena_reset((TetherArena*)alloc->ctx);
}

TetherAllocator tether_arena_allocator(TetherArena* arena) {
    TetherAllocator a;
    a.ctx    = arena;
    a.alloc  = tether_arena_alloc_fn;
    a.realloc = tether_arena_realloc_fn;
    a.free   = tether_arena_free_fn;
    a.reset  = tether_arena_reset_fn;
    return a;
}

/* ============================================================================
 * Fixed-buffer allocator implementation
 * ============================================================================ */

void tether_fixed_buffer_init(TetherFixedBuffer* fb, void* buffer, int64_t capacity) {
    fb->buffer   = (char*)buffer;
    fb->capacity = capacity;
    fb->offset   = 0;
}

void* tether_fixed_buffer_alloc(TetherFixedBuffer* fb, int64_t size) {
    if (!fb || !fb->buffer || size <= 0) return NULL;

    int64_t aligned_offset = align_up(fb->offset, 8);

    if (aligned_offset + size > fb->capacity) {
        return NULL;  /* Buffer exhausted */
    }

    void* result = fb->buffer + aligned_offset;
    fb->offset = aligned_offset + size;
    return result;
}

void tether_fixed_buffer_free(TetherFixedBuffer* fb, void* ptr, int64_t size) {
    /* Individual frees are no-ops for fixed-buffer allocation */
    (void)fb;
    (void)ptr;
    (void)size;
}

void tether_fixed_buffer_reset(TetherFixedBuffer* fb) {
    if (fb) {
        fb->offset = 0;
    }
}

/* Fixed-buffer vtable functions */
static void* tether_fixed_buffer_alloc_fn(TetherAllocator* alloc, size_t size) {
    return tether_fixed_buffer_alloc((TetherFixedBuffer*)alloc->ctx, (int64_t)size);
}

static void* tether_fixed_buffer_realloc_fn(TetherAllocator* alloc, void* ptr,
                                            size_t old_size, size_t new_size) {
    (void)old_size;
    void* new_ptr = tether_fixed_buffer_alloc((TetherFixedBuffer*)alloc->ctx, (int64_t)new_size);
    if (new_ptr && ptr) {
        memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    }
    return new_ptr;
}

static void tether_fixed_buffer_free_fn(TetherAllocator* alloc, void* ptr, size_t size) {
    tether_fixed_buffer_free((TetherFixedBuffer*)alloc->ctx, ptr, (int64_t)size);
}

static void tether_fixed_buffer_reset_fn(TetherAllocator* alloc) {
    tether_fixed_buffer_reset((TetherFixedBuffer*)alloc->ctx);
}

TetherAllocator tether_fixed_buffer_allocator(TetherFixedBuffer* fb) {
    TetherAllocator a;
    a.ctx    = fb;
    a.alloc  = tether_fixed_buffer_alloc_fn;
    a.realloc = tether_fixed_buffer_realloc_fn;
    a.free   = tether_fixed_buffer_free_fn;
    a.reset  = tether_fixed_buffer_reset_fn;
    return a;
}

/* ============================================================================
 * Heap allocator implementation
 * ============================================================================ */

void* tether_heap_alloc(TetherAllocator* alloc, size_t size) {
    (void)alloc;
    if (size == 0) return NULL;
    return malloc(size);
}

void* tether_heap_realloc(TetherAllocator* alloc, void* ptr, size_t old_size, size_t new_size) {
    (void)alloc;
    (void)old_size;
    if (new_size == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, new_size);
}

void tether_heap_free(TetherAllocator* alloc, void* ptr, size_t size) {
    (void)alloc;
    (void)size;
    free(ptr);
}

void tether_heap_reset(TetherAllocator* alloc) {
    /* Heap allocator doesn't support bulk reset */
    (void)alloc;
}

TetherAllocator tether_heap_allocator(void) {
    TetherAllocator a;
    a.ctx    = NULL;
    a.alloc  = tether_heap_alloc;
    a.realloc = tether_heap_realloc;
    a.free   = tether_heap_free;
    a.reset  = tether_heap_reset;
    return a;
}

/* ============================================================================
 * Box operations
 * ============================================================================ */

TetherBox tether_box_new(const void* data, int64_t size) {
    TetherBox box;
    if (size <= 0 || !data) {
        box.ptr  = NULL;
        box.size = 0;
        return box;
    }

    box.ptr = malloc((size_t)size);
    if (box.ptr) {
        memcpy(box.ptr, data, (size_t)size);
        box.size = size;
    } else {
        box.size = 0;
    }
    return box;
}

void tether_box_drop(TetherBox* box) {
    if (box && box->ptr) {
        free(box->ptr);
        box->ptr  = NULL;
        box->size = 0;
    }
}

void* tether_box_deref(TetherBox* box) {
    if (!box) return NULL;
    return box->ptr;
}

/* ============================================================================
 * Rc operations (non-atomic reference counting)
 *
 * Memory layout:  { int64_t refcount; char data[data_size]; }
 * ============================================================================ */

TetherRc tether_rc_new(const void* data, int64_t size) {
    TetherRc rc;
    if (size <= 0 || !data) {
        rc.ptr       = NULL;
        rc.data_size = 0;
        return rc;
    }

    /* Allocate: refcount (8 bytes) + data */
    int64_t total = sizeof(int64_t) + size;
    void* block = malloc((size_t)total);
    if (!block) {
        rc.ptr       = NULL;
        rc.data_size = 0;
        return rc;
    }

    /* Set refcount to 1 */
    int64_t* refcount_ptr = (int64_t*)block;
    *refcount_ptr = 1;

    /* Copy data after the refcount */
    char* data_ptr = (char*)block + sizeof(int64_t);
    memcpy(data_ptr, data, (size_t)size);

    rc.ptr       = block;
    rc.data_size = size;
    return rc;
}

TetherRc tether_rc_clone(TetherRc* rc) {
    TetherRc result;
    result.ptr       = NULL;
    result.data_size = 0;

    if (!rc || !rc->ptr) return result;

    /* Increment the refcount (non-atomic) */
    int64_t* refcount_ptr = (int64_t*)rc->ptr;
    (*refcount_ptr)++;

    result.ptr       = rc->ptr;
    result.data_size = rc->data_size;
    return result;
}

void tether_rc_drop(TetherRc* rc) {
    if (!rc || !rc->ptr) return;

    int64_t* refcount_ptr = (int64_t*)rc->ptr;
    (*refcount_ptr)--;

    if (*refcount_ptr <= 0) {
        free(rc->ptr);
    }

    rc->ptr       = NULL;
    rc->data_size = 0;
}

void* tether_rc_deref(TetherRc* rc) {
    if (!rc || !rc->ptr) return NULL;
    return (char*)rc->ptr + sizeof(int64_t);
}

int64_t tether_rc_count(TetherRc* rc) {
    if (!rc || !rc->ptr) return 0;
    return *(int64_t*)rc->ptr;
}

/* ============================================================================
 * Arc operations (atomic reference counting)
 *
 * Memory layout:  { _Atomic int64_t refcount; char data[data_size]; }
 *
 * Memory ordering choices (following Rust's std::sync::Arc):
 *
 *   - arc_new:   Relaxed store — we are the sole owner at creation time;
 *                no other thread can observe the refcount yet.
 *
 *   - arc_clone: Relaxed fetch_add — incrementing only needs atomicity, not
 *                ordering; we don't need to synchronise any other memory.
 *
 *   - arc_drop:  AcqRel fetch_sub — the Release part ensures all prior writes
 *                (to the data) are visible to the thread that frees the block;
 *                the Acquire part ensures the last dropper sees all prior
 *                accesses from every clone/drop that preceded it.
 *
 *   - arc_count: Relaxed load — this is a diagnostic-only read with no
 *                synchronisation requirements; the value is inherently
 *                approximate since another thread may change it immediately.
 * ============================================================================ */

TetherArc tether_arc_new(const void* data, int64_t size) {
    TetherArc arc;
    if (size <= 0 || !data) {
        arc.ptr       = NULL;
        arc.data_size = 0;
        return arc;
    }

    int64_t total = sizeof(_Atomic int64_t) + size;
    void* block = malloc((size_t)total);
    if (!block) {
        arc.ptr       = NULL;
        arc.data_size = 0;
        return arc;
    }

    /* Set refcount to 1 — Relaxed: sole owner, no other thread can observe */
    _Atomic int64_t* refcount_ptr = (_Atomic int64_t*)block;
    __atomic_store_n(refcount_ptr, 1, __ATOMIC_RELAXED);

    /* Copy data after the refcount */
    char* data_ptr = (char*)block + sizeof(_Atomic int64_t);
    memcpy(data_ptr, data, (size_t)size);

    arc.ptr       = block;
    arc.data_size = size;
    return arc;
}

TetherArc tether_arc_clone(TetherArc* arc) {
    TetherArc result;
    result.ptr       = NULL;
    result.data_size = 0;

    if (!arc || !arc->ptr) return result;

    /* Atomically increment the refcount — Relaxed: only atomicity needed */
    _Atomic int64_t* refcount_ptr = (_Atomic int64_t*)arc->ptr;
    __atomic_fetch_add(refcount_ptr, 1, __ATOMIC_RELAXED);

    result.ptr       = arc->ptr;
    result.data_size = arc->data_size;
    return result;
}

void tether_arc_drop(TetherArc* arc) {
    if (!arc || !arc->ptr) return;

    /* Atomically decrement the refcount — AcqRel: must synchronise with last dropper */
    _Atomic int64_t* refcount_ptr = (_Atomic int64_t*)arc->ptr;
    int64_t old_count = __atomic_fetch_sub(refcount_ptr, 1, __ATOMIC_ACQ_REL);

    if (old_count <= 1) {
        /* This was the last reference; free the block */
        free(arc->ptr);
    }

    arc->ptr       = NULL;
    arc->data_size = 0;
}

void* tether_arc_deref(TetherArc* arc) {
    if (!arc || !arc->ptr) return NULL;
    return (char*)arc->ptr + sizeof(_Atomic int64_t);
}

int64_t tether_arc_count(TetherArc* arc) {
    if (!arc || !arc->ptr) return 0;
    _Atomic int64_t* refcount_ptr = (_Atomic int64_t*)arc->ptr;
    return __atomic_load_n(refcount_ptr, __ATOMIC_RELAXED);
}

/* ============================================================================
 * Print functions
 * ============================================================================ */

void tether_print_i32(int32_t value) {
    printf("%d", value);
}

void tether_print_i64(int64_t value) {
    printf("%ld", value);
}

void tether_print_f32(float value) {
    printf("%.6g", (double)value);
}

void tether_print_f64(double value) {
    printf("%.15g", value);
}

void tether_print_bool(bool value) {
    printf("%s", value ? "true" : "false");
}

void tether_print_str(const char* str, int64_t len) {
    if (str && len > 0) {
        printf("%.*s", (int)len, str);
    }
}

void tether_print_ln(void) {
    printf("\n");
}
