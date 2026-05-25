/**
 * jules_runtime.c - Runtime implementation for the Jules programming language
 *
 * Implements all the runtime functions declared in jules_runtime.h:
 *   - Arena allocator
 *   - Fixed-buffer allocator
 *   - Heap allocator
 *   - Box operations
 *   - Rc operations (non-atomic reference counting)
 *   - Arc operations (atomic reference counting)
 *   - Print functions
 */

#include "jules_runtime.h"

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

void jules_arena_init(JulesArena* arena, void* buffer, int64_t capacity) {
    arena->buffer   = (char*)buffer;
    arena->capacity = capacity;
    arena->offset   = 0;
}

void* jules_arena_alloc(JulesArena* arena, int64_t size) {
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

void jules_arena_free(JulesArena* arena, void* ptr, int64_t size) {
    /* Individual frees are no-ops for arena allocation */
    (void)arena;
    (void)ptr;
    (void)size;
}

void jules_arena_reset(JulesArena* arena) {
    if (arena) {
        arena->offset = 0;
    }
}

/* Arena vtable functions */
static void* jules_arena_alloc_fn(JulesAllocator* alloc, size_t size) {
    return jules_arena_alloc((JulesArena*)alloc->ctx, (int64_t)size);
}

static void* jules_arena_realloc_fn(JulesAllocator* alloc, void* ptr,
                                     size_t old_size, size_t new_size) {
    /* Arena doesn't support individual realloc; allocate new and copy */
    (void)old_size;
    void* new_ptr = jules_arena_alloc((JulesArena*)alloc->ctx, (int64_t)new_size);
    if (new_ptr && ptr) {
        memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    }
    return new_ptr;
}

static void jules_arena_free_fn(JulesAllocator* alloc, void* ptr, size_t size) {
    jules_arena_free((JulesArena*)alloc->ctx, ptr, (int64_t)size);
}

static void jules_arena_reset_fn(JulesAllocator* alloc) {
    jules_arena_reset((JulesArena*)alloc->ctx);
}

JulesAllocator jules_arena_allocator(JulesArena* arena) {
    JulesAllocator a;
    a.ctx    = arena;
    a.alloc  = jules_arena_alloc_fn;
    a.realloc = jules_arena_realloc_fn;
    a.free   = jules_arena_free_fn;
    a.reset  = jules_arena_reset_fn;
    return a;
}

/* ============================================================================
 * Fixed-buffer allocator implementation
 * ============================================================================ */

void jules_fixed_buffer_init(JulesFixedBuffer* fb, void* buffer, int64_t capacity) {
    fb->buffer   = (char*)buffer;
    fb->capacity = capacity;
    fb->offset   = 0;
}

void* jules_fixed_buffer_alloc(JulesFixedBuffer* fb, int64_t size) {
    if (!fb || !fb->buffer || size <= 0) return NULL;

    int64_t aligned_offset = align_up(fb->offset, 8);

    if (aligned_offset + size > fb->capacity) {
        return NULL;  /* Buffer exhausted */
    }

    void* result = fb->buffer + aligned_offset;
    fb->offset = aligned_offset + size;
    return result;
}

void jules_fixed_buffer_free(JulesFixedBuffer* fb, void* ptr, int64_t size) {
    /* Individual frees are no-ops for fixed-buffer allocation */
    (void)fb;
    (void)ptr;
    (void)size;
}

void jules_fixed_buffer_reset(JulesFixedBuffer* fb) {
    if (fb) {
        fb->offset = 0;
    }
}

/* Fixed-buffer vtable functions */
static void* jules_fixed_buffer_alloc_fn(JulesAllocator* alloc, size_t size) {
    return jules_fixed_buffer_alloc((JulesFixedBuffer*)alloc->ctx, (int64_t)size);
}

static void* jules_fixed_buffer_realloc_fn(JulesAllocator* alloc, void* ptr,
                                            size_t old_size, size_t new_size) {
    (void)old_size;
    void* new_ptr = jules_fixed_buffer_alloc((JulesFixedBuffer*)alloc->ctx, (int64_t)new_size);
    if (new_ptr && ptr) {
        memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    }
    return new_ptr;
}

static void jules_fixed_buffer_free_fn(JulesAllocator* alloc, void* ptr, size_t size) {
    jules_fixed_buffer_free((JulesFixedBuffer*)alloc->ctx, ptr, (int64_t)size);
}

static void jules_fixed_buffer_reset_fn(JulesAllocator* alloc) {
    jules_fixed_buffer_reset((JulesFixedBuffer*)alloc->ctx);
}

JulesAllocator jules_fixed_buffer_allocator(JulesFixedBuffer* fb) {
    JulesAllocator a;
    a.ctx    = fb;
    a.alloc  = jules_fixed_buffer_alloc_fn;
    a.realloc = jules_fixed_buffer_realloc_fn;
    a.free   = jules_fixed_buffer_free_fn;
    a.reset  = jules_fixed_buffer_reset_fn;
    return a;
}

/* ============================================================================
 * Heap allocator implementation
 * ============================================================================ */

void* jules_heap_alloc(JulesAllocator* alloc, size_t size) {
    (void)alloc;
    if (size == 0) return NULL;
    return malloc(size);
}

void* jules_heap_realloc(JulesAllocator* alloc, void* ptr, size_t old_size, size_t new_size) {
    (void)alloc;
    (void)old_size;
    if (new_size == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, new_size);
}

void jules_heap_free(JulesAllocator* alloc, void* ptr, size_t size) {
    (void)alloc;
    (void)size;
    free(ptr);
}

void jules_heap_reset(JulesAllocator* alloc) {
    /* Heap allocator doesn't support bulk reset */
    (void)alloc;
}

JulesAllocator jules_heap_allocator(void) {
    JulesAllocator a;
    a.ctx    = NULL;
    a.alloc  = jules_heap_alloc;
    a.realloc = jules_heap_realloc;
    a.free   = jules_heap_free;
    a.reset  = jules_heap_reset;
    return a;
}

/* ============================================================================
 * Box operations
 * ============================================================================ */

JulesBox jules_box_new(const void* data, int64_t size) {
    JulesBox box;
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

void jules_box_drop(JulesBox* box) {
    if (box && box->ptr) {
        free(box->ptr);
        box->ptr  = NULL;
        box->size = 0;
    }
}

void* jules_box_deref(JulesBox* box) {
    if (!box) return NULL;
    return box->ptr;
}

/* ============================================================================
 * Rc operations (non-atomic reference counting)
 *
 * Memory layout:  { int64_t refcount; char data[data_size]; }
 * ============================================================================ */

JulesRc jules_rc_new(const void* data, int64_t size) {
    JulesRc rc;
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

JulesRc jules_rc_clone(JulesRc* rc) {
    JulesRc result;
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

void jules_rc_drop(JulesRc* rc) {
    if (!rc || !rc->ptr) return;

    int64_t* refcount_ptr = (int64_t*)rc->ptr;
    (*refcount_ptr)--;

    if (*refcount_ptr <= 0) {
        free(rc->ptr);
    }

    rc->ptr       = NULL;
    rc->data_size = 0;
}

void* jules_rc_deref(JulesRc* rc) {
    if (!rc || !rc->ptr) return NULL;
    return (char*)rc->ptr + sizeof(int64_t);
}

int64_t jules_rc_count(JulesRc* rc) {
    if (!rc || !rc->ptr) return 0;
    return *(int64_t*)rc->ptr;
}

/* ============================================================================
 * Arc operations (atomic reference counting)
 *
 * Memory layout:  { _Atomic int64_t refcount; char data[data_size]; }
 * ============================================================================ */

JulesArc jules_arc_new(const void* data, int64_t size) {
    JulesArc arc;
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

    /* Set refcount to 1 (atomic store) */
    _Atomic int64_t* refcount_ptr = (_Atomic int64_t*)block;
    __atomic_store_n(refcount_ptr, 1, __ATOMIC_SEQ_CST);

    /* Copy data after the refcount */
    char* data_ptr = (char*)block + sizeof(_Atomic int64_t);
    memcpy(data_ptr, data, (size_t)size);

    arc.ptr       = block;
    arc.data_size = size;
    return arc;
}

JulesArc jules_arc_clone(JulesArc* arc) {
    JulesArc result;
    result.ptr       = NULL;
    result.data_size = 0;

    if (!arc || !arc->ptr) return result;

    /* Atomically increment the refcount */
    _Atomic int64_t* refcount_ptr = (_Atomic int64_t*)arc->ptr;
    __atomic_fetch_add(refcount_ptr, 1, __ATOMIC_SEQ_CST);

    result.ptr       = arc->ptr;
    result.data_size = arc->data_size;
    return result;
}

void jules_arc_drop(JulesArc* arc) {
    if (!arc || !arc->ptr) return;

    /* Atomically decrement the refcount */
    _Atomic int64_t* refcount_ptr = (_Atomic int64_t*)arc->ptr;
    int64_t old_count = __atomic_fetch_sub(refcount_ptr, 1, __ATOMIC_SEQ_CST);

    if (old_count <= 1) {
        /* This was the last reference; free the block */
        free(arc->ptr);
    }

    arc->ptr       = NULL;
    arc->data_size = 0;
}

void* jules_arc_deref(JulesArc* arc) {
    if (!arc || !arc->ptr) return NULL;
    return (char*)arc->ptr + sizeof(_Atomic int64_t);
}

int64_t jules_arc_count(JulesArc* arc) {
    if (!arc || !arc->ptr) return 0;
    _Atomic int64_t* refcount_ptr = (_Atomic int64_t*)arc->ptr;
    return __atomic_load_n(refcount_ptr, __ATOMIC_SEQ_CST);
}

/* ============================================================================
 * Print functions
 * ============================================================================ */

void jules_print_i32(int32_t value) {
    printf("%d", value);
}

void jules_print_i64(int64_t value) {
    printf("%ld", value);
}

void jules_print_f32(float value) {
    printf("%.6g", (double)value);
}

void jules_print_f64(double value) {
    printf("%.15g", value);
}

void jules_print_bool(bool value) {
    printf("%s", value ? "true" : "false");
}

void jules_print_str(const char* str, int64_t len) {
    if (str && len > 0) {
        printf("%.*s", (int)len, str);
    }
}

void jules_print_ln(void) {
    printf("\n");
}
