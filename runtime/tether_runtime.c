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

#define _GNU_SOURCE
#include "tether_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <assert.h>
#include <stdint.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/syscall.h>
#include <linux/futex.h>
#endif

/* ============================================================================
 * Helper: align up to 8-byte boundary (for arena/buffer allocators)
 * ============================================================================ */

static int64_t align_up(int64_t value, int64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

/* ============================================================================
 * Arena allocator implementation
 * ============================================================================ */

void tether_arena_init(TetherArena* arena, void* buffer, int64_t capacity, int64_t alignment) {
    arena->buffer    = (char*)buffer;
    arena->capacity  = capacity;
    arena->offset    = 0;
    arena->alignment = alignment > 0 ? alignment : 16;
}

void* tether_arena_alloc(TetherArena* arena, int64_t size) {
    if (!arena || !arena->buffer || size <= 0) return NULL;

    /* Align the offset using the arena's configured alignment */
    int64_t aligned_offset = align_up(arena->offset, arena->alignment);

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
    return tether_malloc(size);
}

void* tether_heap_realloc(TetherAllocator* alloc, void* ptr, size_t old_size, size_t new_size) {
    (void)alloc;
    (void)old_size;
    if (new_size == 0) {
        tether_free(ptr);
        return NULL;
    }
    return tether_realloc(ptr, new_size);
}

void tether_heap_free(TetherAllocator* alloc, void* ptr, size_t size) {
    (void)alloc;
    (void)size;
    tether_free(ptr);
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
 * Thread-local small-box cache implementation
 * ============================================================================ */

static __thread TetherBoxCache tl_box_cache = { .count = 0, .capacity = TETHER_SMALL_BOX_CACHE_SIZE };

TetherBoxCache* tether_box_cache_get(void) { return &tl_box_cache; }

void* tether_box_cache_alloc(TetherBoxCache* cache, int64_t size) {
    if (cache->count > 0 && size <= TETHER_SMALL_BOX_THRESHOLD) {
        return cache->blocks[--cache->count];
    }
    return tether_malloc((size_t)size);
}

void tether_box_cache_free(TetherBoxCache* cache, void* ptr, int64_t size) {
    if (ptr && cache->count < cache->capacity && size <= TETHER_SMALL_BOX_THRESHOLD) {
        cache->blocks[cache->count++] = ptr;
        return;
    }
    tether_free(ptr);
}

/* ============================================================================
 * Batched allocation (allocation fusion) implementation
 * ============================================================================ */

TetherAllocBatch tether_batch_alloc(int64_t total_size) {
    TetherAllocBatch batch;
    batch.buffer = tether_malloc((size_t)total_size);
    batch.capacity = total_size;
    batch.offset = 0;
    batch.count = 0;
    return batch;
}

void* tether_batch_carve(TetherAllocBatch* batch, int64_t size) {
    if (!batch || !batch->buffer || size <= 0) return NULL;
    int64_t aligned_offset = align_up(batch->offset, 16); /* 16-byte alignment for all types */
    if (aligned_offset + size > batch->capacity) return NULL;
    void* result = (char*)batch->buffer + aligned_offset;
    batch->offset = aligned_offset + size;
    batch->count++;
    return result;
}

void tether_batch_free(TetherAllocBatch* batch) {
    if (batch && batch->buffer) {
        tether_free(batch->buffer);
        batch->buffer = NULL;
        batch->capacity = 0;
        batch->offset = 0;
        batch->count = 0;
    }
}

/* ============================================================================
 * Box operations
 * ============================================================================ */

TetherBox tether_box_new(const void* data, int64_t size) {
    /* Backward compat: delegates to tether_box_new_sized with the given size */
    return tether_box_new_sized(data, size);
}

TetherBox tether_box_new_sized(const void* data, int64_t size) {
    TetherBox box;
    box.ptr = NULL;
    if (size <= 0 || !data) {
        return box;
    }

    TetherBoxCache* cache = tether_box_cache_get();
    box.ptr = (char*)tether_box_cache_alloc(cache, size);
    if (box.ptr) {
        memcpy(box.ptr, data, (size_t)size);
    }
    return box;
}

void tether_box_drop(TetherBox* box) {
    /* Backward compat: uses default size=0 (cache will free via tether_free) */
    tether_box_drop_sized(box, 0);
}

void tether_box_drop_sized(TetherBox* box, int64_t size) {
    if (box && box->ptr) {
        TetherBoxCache* cache = tether_box_cache_get();
        tether_box_cache_free(cache, box->ptr, size);
        box->ptr = NULL;
    }
}

void* tether_box_deref(TetherBox* box) {
    if (!box) return NULL;
    return box->ptr;
}

/* ============================================================================
 * Rc operations (non-atomic reference counting)
 *
 * Memory layout:  { int32_t refcount; char data[data_size]; }
 * ============================================================================ */

TetherRc tether_rc_new(const void* data, int64_t size) {
    TetherRc rc;
    if (size <= 0 || !data) {
        rc.ptr       = NULL;
        rc.data_size = 0;
        return rc;
    }

    /* Allocate: refcount (4 bytes) + data */
    int64_t total = sizeof(int32_t) + size;
    void* block = tether_malloc((size_t)total);
    if (!block) {
        rc.ptr       = NULL;
        rc.data_size = 0;
        return rc;
    }

    /* Set refcount to 1 */
    int32_t* refcount_ptr = (int32_t*)block;
    *refcount_ptr = 1;

    /* Copy data after the refcount */
    char* data_ptr = (char*)block + sizeof(int32_t);
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
    int32_t* refcount_ptr = (int32_t*)rc->ptr;
    (*refcount_ptr)++;

    result.ptr       = rc->ptr;
    result.data_size = rc->data_size;
    return result;
}

void tether_rc_drop(TetherRc* rc) {
    if (!rc || !rc->ptr) return;

    int32_t* refcount_ptr = (int32_t*)rc->ptr;
    (*refcount_ptr)--;

    if (*refcount_ptr <= 0) {
        tether_free(rc->ptr);
    }

    rc->ptr       = NULL;
    rc->data_size = 0;
}

void* tether_rc_deref(TetherRc* rc) {
    if (!rc || !rc->ptr) return NULL;
    return (char*)rc->ptr + sizeof(int32_t);
}

int32_t tether_rc_count(TetherRc* rc) {
    if (!rc || !rc->ptr) return 0;
    return *(int32_t*)rc->ptr;
}

/* ============================================================================
 * Arc operations (atomic reference counting)
 *
 * Memory layout:  { _Atomic int32_t refcount; char data[data_size]; }
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

    int64_t total = sizeof(_Atomic int32_t) + size;
    void* block = tether_malloc((size_t)total);
    if (!block) {
        arc.ptr       = NULL;
        arc.data_size = 0;
        return arc;
    }

    /* Set refcount to 1 — Relaxed: sole owner, no other thread can observe */
    _Atomic int32_t* refcount_ptr = (_Atomic int32_t*)block;
    __atomic_store_n(refcount_ptr, 1, __ATOMIC_RELAXED);

    /* Copy data after the refcount */
    char* data_ptr = (char*)block + sizeof(_Atomic int32_t);
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
    _Atomic int32_t* refcount_ptr = (_Atomic int32_t*)arc->ptr;
    __atomic_fetch_add(refcount_ptr, 1, __ATOMIC_RELAXED);

    result.ptr       = arc->ptr;
    result.data_size = arc->data_size;
    return result;
}

void tether_arc_drop(TetherArc* arc) {
    if (!arc || !arc->ptr) return;

    /* Atomically decrement the refcount — AcqRel: must synchronise with last dropper */
    _Atomic int32_t* refcount_ptr = (_Atomic int32_t*)arc->ptr;
    int32_t old_count = __atomic_fetch_sub(refcount_ptr, 1, __ATOMIC_ACQ_REL);

    if (old_count <= 1) {
        /* This was the last reference; free the block */
        tether_free(arc->ptr);
    }

    arc->ptr       = NULL;
    arc->data_size = 0;
}

void* tether_arc_deref(TetherArc* arc) {
    if (!arc || !arc->ptr) return NULL;
    return (char*)arc->ptr + sizeof(_Atomic int32_t);
}

int32_t tether_arc_count(TetherArc* arc) {
    if (!arc || !arc->ptr) return 0;
    _Atomic int32_t* refcount_ptr = (_Atomic int32_t*)arc->ptr;
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

/* ============================================================================
 * Optimized string operations
 *
 * These functions support the zero-copy string slice and fast string equality
 * optimizations in the Tether compiler:
 *   - tether_str_eq:    O(1) for interned strings, O(n) worst-case
 *   - tether_str_slice: Zero-copy substring (just adjusts pointer + length)
 * ============================================================================ */

/* Fast string equality — checks length first, then pointer equality (for
 * interned strings), then falls back to memcmp. When the Tether compiler
 * interns string literals, identical strings share the same pointer, making
 * this O(1) in the common case. */
bool tether_str_eq(const char* a, int64_t a_len, const char* b, int64_t b_len) {
    if (a_len != b_len) return false;
    if (a == b) return true;  /* Same pointer = interned or same slice */
    return memcmp(a, b, (size_t)a_len) == 0;
}

/* Zero-copy string slice — just adjusts pointer and length.
 * No memcpy, no allocation. The returned slice points into the
 * original string's memory, making substring operations O(1). */
TetherSlice_void tether_str_slice(const char* str, int64_t str_len,
                                   int64_t start, int64_t len) {
    (void)str_len;  /* Total length available for bounds checking in future */
    TetherSlice_void result;
    result.ptr = (void*)(str + start);
    result.len = len;
    return result;
}

void tether_print_ln(void) {
    printf("\n");
}

// ============================================================================
// Spawn / Task Pool Implementation
//
// Uses a simple work-stealing thread pool with:
//   - Global work queue (lock-free single-producer, multi-consumer)
//   - Per-thread local queues (for work stealing)
//   - Condition variable for worker thread wakeup
//   - Atomic reference counting on task handles
// ============================================================================

#ifdef _WIN32
  #include <windows.h>
#else
  #include <pthread.h>
  #include <stdatomic.h>
#endif

// --- Task states ---
#define TETHER_TASK_PENDING  0
#define TETHER_TASK_RUNNING  1
#define TETHER_TASK_DONE     2

struct TetherTask {
    TetherTaskFn   fn;          // The function to execute
    void*          ctx;         // Context pointer (owned copy)
    uint64_t       ctx_size;    // Size of context data
    void*          result;      // Result of the task (set when done)
    atomic_int     state;       // TETHER_TASK_PENDING/RUNNING/DONE
    atomic_int     refcount;    // Reference count for handle lifetime
};

// --- Chase-Lev deque for per-thread work stealing ---
#define CL_DEQUE_CAPACITY 8192

struct ChaseLevDeque {
    // Circular array of task pointers
    TetherTask** buffer;
    int capacity;
    // Bottom is only accessed by the owner thread
    _Atomic int bottom;
    // Top is accessed by thieves via atomic CAS
    _Atomic int top;
};

static void cl_deque_init(struct ChaseLevDeque* dq) {
    dq->capacity = CL_DEQUE_CAPACITY;
    dq->buffer = (TetherTask**)calloc(dq->capacity, sizeof(TetherTask*));
    atomic_store(&dq->bottom, 0);
    atomic_store(&dq->top, 0);
}

static void cl_deque_destroy(struct ChaseLevDeque* dq) {
    free(dq->buffer);
    dq->buffer = NULL;
}

// Push to bottom (owner thread only)
static bool cl_deque_push(struct ChaseLevDeque* dq, TetherTask* task) {
    int b = atomic_load(&dq->bottom);
    int t = atomic_load(&dq->top);
    if (b - t >= dq->capacity) return false; // Full
    dq->buffer[b % dq->capacity] = task;
    atomic_store_explicit(&dq->bottom, b + 1, __ATOMIC_RELEASE);
    return true;
}

// Pop from bottom (owner thread only) — returns NULL if empty
static TetherTask* cl_deque_pop(struct ChaseLevDeque* dq) {
    int b = atomic_load(&dq->bottom) - 1;
    atomic_store(&dq->bottom, b);
    __asm__ __volatile__("" ::: "memory"); // Compiler barrier
    int t = atomic_load(&dq->top);
    if (t > b) {
        // Deque was empty, restore
        atomic_store(&dq->bottom, b + 1);
        return NULL;
    }
    TetherTask* task = dq->buffer[b % dq->capacity];
    if (t == b) {
        // Last element — race with steal
        if (!atomic_compare_exchange_strong(&dq->top, &t, t + 1)) {
            // Steal won the race
            atomic_store(&dq->bottom, b + 1);
            return NULL;
        }
        atomic_store(&dq->bottom, b + 1);
    }
    return task;
}

// Steal from top (thief thread) — returns NULL if empty
static TetherTask* cl_deque_steal(struct ChaseLevDeque* dq) {
    int t = atomic_load(&dq->top);
    __asm__ __volatile__("" ::: "memory"); // Compiler barrier
    int b = atomic_load(&dq->bottom);
    if (t >= b) return NULL; // Empty
    TetherTask* task = dq->buffer[t % dq->capacity];
    if (!atomic_compare_exchange_strong(&dq->top, &t, t + 1)) {
        return NULL; // CAS failed, another thief got it
    }
    return task;
}

// --- Global work queue (simple locked deque) ---
#define TETHER_MAX_QUEUED_TASKS 65536

// --- Adaptive spin parameters (configurable) ---
#define TETHER_DEFAULT_SPIN_COUNT   100    // Pure spin iterations (Phase 1)
#define TETHER_DEFAULT_YIELD_COUNT  1000   // sched_yield iterations (Phase 2)

static struct {
    TetherTask*    tasks[TETHER_MAX_QUEUED_TASKS];
    atomic_int     head;
    atomic_int     tail;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    int             shutdown;
    pthread_t*      threads;
    uint32_t        num_threads;
    int             initialized;
    // Adaptive spin parameters for tether_task_await()
    uint32_t        spin_count;    // Phase 1: pure CPU spin iterations
    uint32_t        yield_count;   // Phase 2: sched_yield iterations
    // Per-thread Chase-Lev deques for work stealing
    struct ChaseLevDeque* local_deques;
} g_taskpool;

// --- Worker thread function ---
static void* tether_worker_thread(void* arg) {
    int worker_idx = (int)(intptr_t)arg;
    while (1) {
        TetherTask* task = NULL;

        // Pop from local deque first
        task = cl_deque_pop(&g_taskpool.local_deques[worker_idx]);

        // Try to steal from other workers
        if (!task) {
            for (int i = 0; i < (int)g_taskpool.num_threads; i++) {
                if (i == worker_idx) continue;
                task = cl_deque_steal(&g_taskpool.local_deques[i]);
                if (task) break;
            }
        }

        // If still no task, fall back to global queue
        if (!task) {
            pthread_mutex_lock(&g_taskpool.lock);

            // Wait for tasks
            while (atomic_load(&g_taskpool.head) >= atomic_load(&g_taskpool.tail)
                   && !g_taskpool.shutdown) {
                pthread_cond_wait(&g_taskpool.not_empty, &g_taskpool.lock);
            }

            if (g_taskpool.shutdown &&
                atomic_load(&g_taskpool.head) >= atomic_load(&g_taskpool.tail)) {
                pthread_mutex_unlock(&g_taskpool.lock);
                break;
            }

            // Dequeue a task from global queue
            int head = atomic_load(&g_taskpool.head);
            int tail = atomic_load(&g_taskpool.tail);
            if (head < tail) {
                task = g_taskpool.tasks[head % TETHER_MAX_QUEUED_TASKS];
                atomic_store(&g_taskpool.head, head + 1);
            }

            pthread_mutex_unlock(&g_taskpool.lock);
        }

        // Execute the task
        if (task) {
            atomic_store(&task->state, TETHER_TASK_RUNNING);
            task->result = task->fn(task->ctx);
            atomic_store(&task->state, TETHER_TASK_DONE);
            // Futex wake on Linux — much more efficient than cond var broadcast
#if defined(__linux__)
            syscall(SYS_futex, &task->state, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
#endif
            // Signal any threads that might be waiting in tether_task_await
            // (Fallback for non-Linux Phase 3 blocking on cond var)
            pthread_cond_broadcast(&g_taskpool.not_empty);
        }
    }
    return NULL;
}

void tether_taskpool_init(uint32_t num_threads) {
    if (g_taskpool.initialized) return;

    if (num_threads == 0) {
        // Default to number of hardware threads, minimum 2
        long hw = sysconf(_SC_NPROCESSORS_ONLN);
        num_threads = (hw > 1) ? (uint32_t)hw : 2;
    }

    g_taskpool.num_threads = num_threads;
    g_taskpool.shutdown = 0;
    g_taskpool.spin_count = TETHER_DEFAULT_SPIN_COUNT;
    g_taskpool.yield_count = TETHER_DEFAULT_YIELD_COUNT;
    atomic_store(&g_taskpool.head, 0);
    atomic_store(&g_taskpool.tail, 0);
    pthread_mutex_init(&g_taskpool.lock, NULL);
    pthread_cond_init(&g_taskpool.not_empty, NULL);

    // Allocate and init per-thread Chase-Lev deques
    g_taskpool.local_deques = (struct ChaseLevDeque*)calloc(num_threads, sizeof(struct ChaseLevDeque));
    for (uint32_t i = 0; i < num_threads; i++) {
        cl_deque_init(&g_taskpool.local_deques[i]);
    }

    g_taskpool.threads = (pthread_t*)malloc(sizeof(pthread_t) * num_threads);
    for (uint32_t i = 0; i < num_threads; i++) {
        pthread_create(&g_taskpool.threads[i], NULL, tether_worker_thread, (void*)(intptr_t)i);
    }

    g_taskpool.initialized = 1;
}

void tether_taskpool_shutdown(void) {
    if (!g_taskpool.initialized) return;

    pthread_mutex_lock(&g_taskpool.lock);
    g_taskpool.shutdown = 1;
    pthread_cond_broadcast(&g_taskpool.not_empty);
    pthread_mutex_unlock(&g_taskpool.lock);

    for (uint32_t i = 0; i < g_taskpool.num_threads; i++) {
        pthread_join(g_taskpool.threads[i], NULL);
    }

    // Destroy per-thread Chase-Lev deques
    if (g_taskpool.local_deques) {
        for (uint32_t i = 0; i < g_taskpool.num_threads; i++) {
            cl_deque_destroy(&g_taskpool.local_deques[i]);
        }
        free(g_taskpool.local_deques);
        g_taskpool.local_deques = NULL;
    }

    free(g_taskpool.threads);
    g_taskpool.threads = NULL;
    g_taskpool.num_threads = 0;
    g_taskpool.initialized = 0;

    pthread_mutex_destroy(&g_taskpool.lock);
    pthread_cond_destroy(&g_taskpool.not_empty);
}

TetherTaskHandle tether_spawn(TetherTaskFn fn, void* ctx, uint64_t ctx_size) {
    if (!g_taskpool.initialized) {
        tether_taskpool_init(0); // Auto-initialize
    }

    TetherTask* task = (TetherTask*)malloc(sizeof(TetherTask));
    task->fn = fn;
    task->result = NULL;
    atomic_store(&task->state, TETHER_TASK_PENDING);
    atomic_store(&task->refcount, 1);

    // Copy context data so it outlives the caller's stack
    if (ctx && ctx_size > 0) {
        task->ctx = malloc(ctx_size);
        memcpy(task->ctx, ctx, ctx_size);
        task->ctx_size = ctx_size;
    } else {
        task->ctx = ctx;
        task->ctx_size = 0;
    }

    // Round-robin assignment to worker deques
    static _Atomic int next_worker = 0;
    int worker_idx = atomic_fetch_add(&next_worker, 1) % (int)g_taskpool.num_threads;
    if (!cl_deque_push(&g_taskpool.local_deques[worker_idx], task)) {
        // Deque full — fall back to global queue
        pthread_mutex_lock(&g_taskpool.lock);
        int tail = atomic_load(&g_taskpool.tail);
        g_taskpool.tasks[tail % TETHER_MAX_QUEUED_TASKS] = task;
        atomic_store(&g_taskpool.tail, tail + 1);
        pthread_cond_signal(&g_taskpool.not_empty);
        pthread_mutex_unlock(&g_taskpool.lock);
    }

    TetherTaskHandle handle;
    handle.task = task;
    return handle;
}

void* tether_task_await(TetherTaskHandle handle) {
    if (!handle.task) return NULL;

    // Adaptive spin strategy:
    // Phase 1: Spin for spin_count iterations (low latency for short tasks)
    // Phase 2: futex wait on Linux / pthread_cond_wait on other platforms
    //
    // This gives nanosecond latency for HFT (short tasks done in Phase 1)
    // while not burning CPU for general workloads (long tasks block in Phase 2).

    uint32_t spin_count = g_taskpool.spin_count;

    // Phase 1: Pure spin (for very short tasks — nanosecond latency)
    for (uint32_t i = 0; i < spin_count; i++) {
        if (atomic_load(&handle.task->state) == TETHER_TASK_DONE) {
            goto done;
        }
        // CPU pause hint for spin-wait loops — reduces power consumption
        // and improves performance on hyperthreaded CPUs
#if defined(__x86_64__) || defined(_M_X64)
        __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
        __asm__ __volatile__("yield" ::: "memory");
#elif defined(__riscv)
        __asm__ __volatile__("pause" ::: "memory");
#else
        // No pause instruction available — rely on compiler barrier
        __asm__ __volatile__("" ::: "memory");
#endif
    }

    // Phase 2: futex wait (Linux) — much more efficient than spin+yield
#if defined(__linux__)
    {
        // Wait on the task's state field using futex
        // The worker thread will do a FUTEX_WAKE when the task completes
        while (atomic_load(&handle.task->state) != TETHER_TASK_DONE) {
            // futex(FUTEX_WAIT_PRIVATE) — waits until woken or spuriously
            syscall(SYS_futex, &handle.task->state, FUTEX_WAIT_PRIVATE,
                    TETHER_TASK_RUNNING, NULL, NULL, 0);
        }
    }
#else
    // Fallback: pthread_cond_wait for non-Linux
    {
        pthread_mutex_lock(&g_taskpool.lock);
        while (atomic_load(&handle.task->state) != TETHER_TASK_DONE) {
            pthread_cond_wait(&g_taskpool.not_empty, &g_taskpool.lock);
        }
        pthread_mutex_unlock(&g_taskpool.lock);
    }
#endif

done:
    {
        void* result = handle.task->result;

        // Clean up context copy
        if (handle.task->ctx && handle.task->ctx_size > 0) {
            free(handle.task->ctx);
        }

        free(handle.task);
        return result;
    }
}

int tether_task_is_done(TetherTaskHandle handle) {
    if (!handle.task) return 1;
    return atomic_load(&handle.task->state) == TETHER_TASK_DONE ? 1 : 0;
}

uint32_t tether_taskpool_thread_count(void) {
    return g_taskpool.num_threads;
}

void tether_taskpool_set_spin_params(uint32_t spin_count, uint32_t yield_count) {
    g_taskpool.spin_count = spin_count;
    g_taskpool.yield_count = yield_count;
}

/* ============================================================================
 * Deoptimization support (Nuclear #8: Speculative Optimization)
 * ============================================================================ */

/* Global deopt handler callback (NULL = use default handler) */
static TetherDeoptCallback g_deopt_callback = NULL;

/* Deoptimization reason strings (indexed by deopt_id) */
static const char* g_deopt_reasons[] = {
    "unknown deoptimization",
    "null pointer dereference (NeverNull assumption violated)",
    "branch taken (BranchNeverTaken assumption violated)",
    "array index out of bounds (BoundsInRange assumption violated)",
    "arithmetic overflow (NoOverflow assumption violated)",
    "type mismatch (TypeExact assumption violated)",
    "aliasing conflict (NoAlias assumption violated)",
    "side effect in pure call (PureCall assumption violated)",
};

void tether_deopt(uint64_t deopt_id, void* frame) {
    if (g_deopt_callback) {
        TetherDeoptInfo info;
        info.deopt_id = deopt_id;
        info.frame = frame;
        info.reason = (deopt_id < sizeof(g_deopt_reasons) / sizeof(g_deopt_reasons[0]))
                       ? g_deopt_reasons[deopt_id]
                       : "unknown deoptimization reason";
        g_deopt_callback(&info);
        /* If the custom handler returns, we still abort because the
         * deoptimization point's fast-path code is unreachable after this.
         * In a JIT compiler, the handler would never return. */
    }

    /* Default handler: print error and abort */
    const char* reason = (deopt_id < sizeof(g_deopt_reasons) / sizeof(g_deopt_reasons[0]))
                          ? g_deopt_reasons[deopt_id]
                          : "unknown deoptimization reason";
    fprintf(stderr, "[tether] DEOPTIMIZATION: assumption violated at deopt point %lu: %s\n",
            (unsigned long)deopt_id, reason);
    fprintf(stderr, "[tether] This means a speculative optimization was incorrect.\n");
    fprintf(stderr, "[tether] Recompile without speculative optimization or fix the code.\n");
    abort();
}

void tether_register_deopt_handler(TetherDeoptCallback callback) {
    g_deopt_callback = callback;
}

/* ============================================================================
 * Benchmarking intrinsics — black_box and volatile read/write
 *
 * These functions act as optimization barriers, preventing the compiler from
 * eliminating benchmark code. They are essential for accurate microbenchmarks.
 * ============================================================================ */

/* tether_black_box_i64 — consume an i64 value without allowing the optimizer
 * to eliminate it. Uses inline asm with a memory clobber as an optimization
 * fence. The value is stored to a stack slot via the "r" constraint, forcing
 * the compiler to keep it in a register (and thus compute it). */
void tether_black_box_i64(int64_t value) {
    __asm__ __volatile__("" : "+r"(value) :: "memory");
    (void)value;
}

/* tether_black_box_f64 — consume a double value without allowing the
 * optimizer to eliminate it. */
void tether_black_box_f64(double value) {
    __asm__ __volatile__("" : "+x"(value) :: "memory");
    (void)value;
}

/* tether_black_box_ptr — consume a pointer value without allowing the
 * optimizer to eliminate it. */
void tether_black_box_ptr(const void* ptr) {
    __asm__ __volatile__("" : "+r"(ptr) :: "memory");
    (void)ptr;
}

/* tether_volatile_read_i64 — read an i64 from memory with volatile semantics.
 * Forces an actual load (no CSE, no hoisting). */
int64_t tether_volatile_read_i64(const volatile int64_t* ptr) {
    return *ptr;
}

/* tether_volatile_read_f64 — read a double from memory with volatile semantics. */
double tether_volatile_read_f64(const volatile double* ptr) {
    return *ptr;
}

/* tether_volatile_write_i64 — write an i64 to memory with volatile semantics.
 * Forces an actual store (no DSE, no sinking). */
void tether_volatile_write_i64(volatile int64_t* ptr, int64_t value) {
    *ptr = value;
}

/* tether_volatile_write_f64 — write a double to memory with volatile semantics. */
void tether_volatile_write_f64(volatile double* ptr, double value) {
    *ptr = value;
}
