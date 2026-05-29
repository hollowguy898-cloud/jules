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
#include <sched.h>
#include <unistd.h>

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

// --- Global work queue (simple locked deque) ---
#define TETHER_MAX_QUEUED_TASKS 65536

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
} g_taskpool;

// --- Worker thread function ---
static void* tether_worker_thread(void* arg) {
    (void)arg;
    while (1) {
        TetherTask* task = NULL;

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

        // Dequeue a task
        int head = atomic_load(&g_taskpool.head);
        int tail = atomic_load(&g_taskpool.tail);
        if (head < tail) {
            task = g_taskpool.tasks[head % TETHER_MAX_QUEUED_TASKS];
            atomic_store(&g_taskpool.head, head + 1);
        }

        pthread_mutex_unlock(&g_taskpool.lock);

        // Execute the task
        if (task) {
            atomic_store(&task->state, TETHER_TASK_RUNNING);
            task->result = task->fn(task->ctx);
            atomic_store(&task->state, TETHER_TASK_DONE);
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
    atomic_store(&g_taskpool.head, 0);
    atomic_store(&g_taskpool.tail, 0);
    pthread_mutex_init(&g_taskpool.lock, NULL);
    pthread_cond_init(&g_taskpool.not_empty, NULL);

    g_taskpool.threads = (pthread_t*)malloc(sizeof(pthread_t) * num_threads);
    for (uint32_t i = 0; i < num_threads; i++) {
        pthread_create(&g_taskpool.threads[i], NULL, tether_worker_thread, NULL);
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

    // Enqueue
    pthread_mutex_lock(&g_taskpool.lock);
    int tail = atomic_load(&g_taskpool.tail);
    g_taskpool.tasks[tail % TETHER_MAX_QUEUED_TASKS] = task;
    atomic_store(&g_taskpool.tail, tail + 1);
    pthread_cond_signal(&g_taskpool.not_empty);
    pthread_mutex_unlock(&g_taskpool.lock);

    TetherTaskHandle handle;
    handle.task = task;
    return handle;
}

void* tether_task_await(TetherTaskHandle handle) {
    if (!handle.task) return NULL;

    // Spin-wait with yield for low latency (suitable for HFT use cases)
    while (atomic_load(&handle.task->state) != TETHER_TASK_DONE) {
#ifdef _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
    }

    void* result = handle.task->result;

    // Clean up context copy
    if (handle.task->ctx && handle.task->ctx_size > 0) {
        free(handle.task->ctx);
    }

    free(handle.task);
    return result;
}

int tether_task_is_done(TetherTaskHandle handle) {
    if (!handle.task) return 1;
    return atomic_load(&handle.task->state) == TETHER_TASK_DONE ? 1 : 0;
}

uint32_t tether_taskpool_thread_count(void) {
    return g_taskpool.num_threads;
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
