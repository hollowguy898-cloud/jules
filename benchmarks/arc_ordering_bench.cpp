// ============================================================================
// Arc Memory Ordering Microbenchmark
// Before: __ATOMIC_SEQ_CST on all Arc operations
// After:  __ATOMIC_RELAXED for clone, __ATOMIC_ACQ_REL for drop
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <pthread.h>

using hrclock_t = std::chrono::high_resolution_clock;
using ms_t = std::chrono::duration<double, std::milli>;

static const int N = 10'000'000;
static const int THREADS = 4;

struct ArcData {
    std::atomic<int64_t> refcount;
    double payload;
};

// OLD: SeqCst clone
static void arc_clone_sc(ArcData* arc) {
    arc->refcount.fetch_add(1, std::memory_order_seq_cst);
}

// NEW: Relaxed clone
static void arc_clone_relaxed(ArcData* arc) {
    arc->refcount.fetch_add(1, std::memory_order_relaxed);
}

// OLD: SeqCst drop
static bool arc_drop_sc(ArcData* arc) {
    int64_t old = arc->refcount.fetch_sub(1, std::memory_order_seq_cst);
    return old <= 1;
}

// NEW: AcqRel drop
static bool arc_drop_acqrel(ArcData* arc) {
    int64_t old = arc->refcount.fetch_sub(1, std::memory_order_acq_rel);
    return old <= 1;
}

struct BenchArgs {
    ArcData* arc;
    int count;
    double elapsed_ms;
    bool use_new;
};

void* bench_thread(void* arg) {
    auto* args = static_cast<BenchArgs*>(arg);
    auto start = hrclock_t::now();
    
    for (int i = 0; i < args->count; i++) {
        if (args->use_new) {
            arc_clone_relaxed(args->arc);
            arc_drop_acqrel(args->arc);
        } else {
            arc_clone_sc(args->arc);
            arc_drop_sc(args->arc);
        }
    }
    
    args->elapsed_ms = ms_t(hrclock_t::now() - start).count();
    return nullptr;
}

int main() {
    printf("=== Arc Memory Ordering Microbenchmark ===\n");
    printf("Operations: %d clone+drop per thread x %d threads\n\n", N, THREADS);
    
    // ---- OLD: SeqCst ----
    ArcData arc_old;
    arc_old.refcount.store(1, std::memory_order_seq_cst);
    arc_old.payload = 42.0;
    
    pthread_t threads[THREADS];
    BenchArgs args_old[THREADS];
    
    for (int t = 0; t < THREADS; t++) {
        args_old[t].arc = &arc_old;
        args_old[t].count = N / THREADS;
        args_old[t].use_new = false;
    }
    
    for (int t = 0; t < THREADS; t++) {
        pthread_create(&threads[t], nullptr, bench_thread, &args_old[t]);
    }
    for (int t = 0; t < THREADS; t++) {
        pthread_join(threads[t], nullptr);
    }
    
    double old_total = 0;
    for (int t = 0; t < THREADS; t++) old_total += args_old[t].elapsed_ms;
    
    // ---- NEW: Relaxed/AcqRel ----
    ArcData arc_new;
    arc_new.refcount.store(1, std::memory_order_relaxed);
    arc_new.payload = 42.0;
    
    BenchArgs args_new[THREADS];
    for (int t = 0; t < THREADS; t++) {
        args_new[t].arc = &arc_new;
        args_new[t].count = N / THREADS;
        args_new[t].use_new = true;
    }
    
    for (int t = 0; t < THREADS; t++) {
        pthread_create(&threads[t], nullptr, bench_thread, &args_new[t]);
    }
    for (int t = 0; t < THREADS; t++) {
        pthread_join(threads[t], nullptr);
    }
    
    double new_total = 0;
    for (int t = 0; t < THREADS; t++) new_total += args_new[t].elapsed_ms;
    
    printf("┌─────────────────────────────┬──────────────┐\n");
    printf("│ Memory Ordering             │ Time (ms)     │\n");
    printf("├─────────────────────────────┼──────────────┤\n");
    printf("│ SeqCst (OLD)                │ %10.2f   │\n", old_total);
    printf("│ Relaxed/AcqRel (NEW)        │ %10.2f   │\n", new_total);
    printf("├─────────────────────────────┼──────────────┤\n");
    printf("│ Speedup                     │ %10.2fx   │\n", old_total / new_total);
    printf("└─────────────────────────────┴──────────────┘\n");
    
    return 0;
}
