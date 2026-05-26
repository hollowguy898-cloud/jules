// ============================================================================
// Tether ECS Benchmark — 5 Million Entities — O3 Optimization
//
// This C++ program represents what the Tether compiler generates from:
//   soa struct Position { x: f64, y: f64, z: f64 }
//   align(64) struct Health { ... }
//   @simd fn update_positions(...)
//
// Key Tether advantages over vanilla Rust:
//   1. `soa struct` auto-generates SoA layout (Rust requires manual work)
//   2. `align(64)` cache-line alignment + automatic prefetch insertion
//   3. `@simd` guarantees LLVM vectorization with metadata
//   4. `try`/`catch` cold-path annotation improves branch prediction
//   5. Pre-LLVM optimization passes (field reorder, defer coalesce, etc.)
//
// Compile: g++ -std=c++17 -O3 -march=native -ffast-math tether_ecs_5m.cpp -o tether_ecs_5m -lm
// Run:     ./tether_ecs_5m
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cmath>
#include <immintrin.h>   // SSE2/AVX for prefetch hints

using hrclock_t = std::chrono::high_resolution_clock;
using ms_t = std::chrono::duration<double, std::milli>;

static const int N = 5'000'000;
static const int ITERS = 50;

// ============================================================================
// SoA Component Storage — what `soa struct` generates in Tether
// Each field gets its own contiguous, cache-line-aligned array.
// ============================================================================
struct alignas(64) PositionSoA {
    double* x;
    double* y;
    double* z;
    int count;
    int capacity;

    static PositionSoA create(int cap) {
        PositionSoA s;
        s.capacity = cap;
        s.count = 0;
        s.x = static_cast<double*>(aligned_alloc(64, cap * sizeof(double)));
        s.y = static_cast<double*>(aligned_alloc(64, cap * sizeof(double)));
        s.z = static_cast<double*>(aligned_alloc(64, cap * sizeof(double)));
        return s;
    }
    void destroy() { free(x); free(y); free(z); }
};

struct alignas(64) VelocitySoA {
    double* dx;
    double* dy;
    double* dz;
    int count;
    int capacity;

    static VelocitySoA create(int cap) {
        VelocitySoA s;
        s.capacity = cap;
        s.count = 0;
        s.dx = static_cast<double*>(aligned_alloc(64, cap * sizeof(double)));
        s.dy = static_cast<double*>(aligned_alloc(64, cap * sizeof(double)));
        s.dz = static_cast<double*>(aligned_alloc(64, cap * sizeof(double)));
        return s;
    }
    void destroy() { free(dx); free(dy); free(dz); }
};

struct alignas(64) HealthSoA {
    double* hp;
    double* max_hp;
    int* alive;          // 0 or 1
    int count;
    int capacity;

    static HealthSoA create(int cap) {
        HealthSoA s;
        s.capacity = cap;
        s.count = 0;
        s.hp = static_cast<double*>(aligned_alloc(64, cap * sizeof(double)));
        s.max_hp = static_cast<double*>(aligned_alloc(64, cap * sizeof(double)));
        s.alive = static_cast<int*>(aligned_alloc(64, cap * sizeof(int)));
        return s;
    }
    void destroy() { free(hp); free(max_hp); free(alive); }
};

struct alignas(64) TagsSoA {
    uint8_t* team;       // 0 or 1
    bool* active;
    int count;
    int capacity;

    static TagsSoA create(int cap) {
        TagsSoA s;
        s.capacity = cap;
        s.count = 0;
        s.team = static_cast<uint8_t*>(aligned_alloc(64, cap * sizeof(uint8_t)));
        s.active = static_cast<bool*>(aligned_alloc(64, cap * sizeof(bool)));
        return s;
    }
    void destroy() { free(team); free(active); }
};

// ============================================================================
// AoS Entity — traditional OOP layout (Rust default)
// ============================================================================
struct Entity {
    double px, py, pz;     // 24 bytes
    double dx, dy, dz;     // 24 bytes
    double hp, max_hp;     // 16 bytes
    int alive;             // 4 bytes + 4 padding
};
// sizeof(Entity) = 72 bytes

// ============================================================================
// System 1: Movement — SoA + @simd + prefetch (Tether style)
//
// Tether's @simd annotation generates:
//   !llvm.loop !{..., !"llvm.loop.vectorize.enable", i1 true}
//   !llvm.loop !{..., !"llvm.loop.vectorize.width", i32 4}
//
// Tether's align(64) + PrefetchInserter pass generates:
//   call void @llvm.prefetch(ptr %next, i32 0, i32 3, i32 1)
// ============================================================================
static void __attribute__((noinline))
movement_system_soa(PositionSoA& pos, VelocitySoA& vel, double dt) {
    const int n = pos.count;
    // Prefetch distance: ~4 cache lines ahead (256 bytes = 32 doubles)
    const int PREFETCH_DIST = 32;
    int i;
    // Main vectorized loop with prefetch
    for (i = 0; i < n; i++) {
        // Tether's PrefetchInserter pass adds this automatically
        if (i + PREFETCH_DIST < n) {
            __builtin_prefetch(&pos.x[i + PREFETCH_DIST], 1, 3);
            __builtin_prefetch(&pos.y[i + PREFETCH_DIST], 1, 3);
            __builtin_prefetch(&pos.z[i + PREFETCH_DIST], 1, 3);
            __builtin_prefetch(&vel.dx[i + PREFETCH_DIST], 0, 3);
            __builtin_prefetch(&vel.dy[i + PREFETCH_DIST], 0, 3);
            __builtin_prefetch(&vel.dz[i + PREFETCH_DIST], 0, 3);
        }
        pos.x[i] += vel.dx[i] * dt;
        pos.y[i] += vel.dy[i] * dt;
        pos.z[i] += vel.dz[i] * dt;
    }
}

// ============================================================================
// System 1b: Movement — AoS (Rust default)
// ============================================================================
static void __attribute__((noinline))
movement_system_aos(Entity* entities, int count, double dt) {
    for (int i = 0; i < count; i++) {
        entities[i].px += entities[i].dx * dt;
        entities[i].py += entities[i].dy * dt;
        entities[i].pz += entities[i].dz * dt;
    }
}

// ============================================================================
// System 2: Health Regen — SoA
// ============================================================================
static void __attribute__((noinline))
health_system_soa(HealthSoA& health, double dt) {
    const int n = health.count;
    const int PREFETCH_DIST = 32;
    for (int i = 0; i < n; i++) {
        if (i + PREFETCH_DIST < n) {
            __builtin_prefetch(&health.hp[i + PREFETCH_DIST], 1, 3);
            __builtin_prefetch(&health.alive[i + PREFETCH_DIST], 0, 3);
        }
        if (health.alive[i]) {
            health.hp[i] += 0.01 * dt;
            if (health.hp[i] > health.max_hp[i]) {
                health.hp[i] = health.max_hp[i];
            }
        }
    }
}

// ============================================================================
// System 2b: Health Regen — AoS
// ============================================================================
static void __attribute__((noinline))
health_system_aos(Entity* entities, int count, double dt) {
    for (int i = 0; i < count; i++) {
        if (entities[i].alive) {
            entities[i].hp += 0.01 * dt;
            if (entities[i].hp > entities[i].max_hp) {
                entities[i].hp = entities[i].max_hp;
            }
        }
    }
}

// ============================================================================
// System 3: Collision — SoA
// ============================================================================
static uint64_t __attribute__((noinline))
collision_system_soa(PositionSoA& pos, HealthSoA& health) {
    uint64_t collisions = 0;
    const double threshold = 1.0;
    const int n = pos.count;
    for (int i = 0; i + 1 < n; i += 2) {
        double dx = pos.x[i] - pos.x[i + 1];
        double dy = pos.y[i] - pos.y[i + 1];
        double dz = pos.z[i] - pos.z[i + 1];
        double dist_sq = dx * dx + dy * dy + dz * dz;
        if (dist_sq < threshold && health.alive[i] && health.alive[i + 1]) {
            health.hp[i] -= 0.1;
            health.hp[i + 1] -= 0.1;
            collisions++;
        }
    }
    return collisions;
}

// ============================================================================
// System 3b: Collision — AoS
// ============================================================================
static uint64_t __attribute__((noinline))
collision_system_aos(Entity* entities, int count) {
    uint64_t collisions = 0;
    const double threshold = 1.0;
    for (int i = 0; i + 1 < count; i += 2) {
        double dx = entities[i].px - entities[i + 1].px;
        double dy = entities[i].py - entities[i + 1].py;
        double dz = entities[i].pz - entities[i + 1].pz;
        double dist_sq = dx * dx + dy * dy + dz * dz;
        if (dist_sq < threshold && entities[i].alive && entities[i + 1].alive) {
            entities[i].hp -= 0.1;
            entities[i + 1].hp -= 0.1;
            collisions++;
        }
    }
    return collisions;
}

// ============================================================================
// System 4: Tag Filter — SoA (branch-heavy)
// Tether's ErrorPathSeparator annotates unlikely branches as cold
// ============================================================================
static uint64_t __attribute__((noinline))
filter_team_active_soa(TagsSoA& tags, HealthSoA& health) {
    uint64_t count = 0;
    const int n = tags.count;
    for (int i = 0; i < n; i++) {
        if (tags.team[i] == 1 && tags.active[i] && health.alive[i] && health.hp[i] > 10.0) {
            count++;
        }
    }
    return count;
}

// ============================================================================
// System 5: Damage — SoA (vectorizable straight-line)
// ============================================================================
static void __attribute__((noinline))
damage_system_soa(HealthSoA& health, double dmg) {
    const int n = health.count;
    const int PREFETCH_DIST = 32;
    for (int i = 0; i < n; i++) {
        if (i + PREFETCH_DIST < n) {
            __builtin_prefetch(&health.hp[i + PREFETCH_DIST], 1, 3);
        }
        if (health.alive[i]) {
            health.hp[i] -= dmg;
        }
    }
}

// ============================================================================
// System 5b: Damage — AoS
// ============================================================================
static void __attribute__((noinline))
damage_system_aos(Entity* entities, int count, double dmg) {
    for (int i = 0; i < count; i++) {
        if (entities[i].alive) {
            entities[i].hp -= dmg;
        }
    }
}

// ============================================================================
// System 6: Sparse Update — SoA (10% of entities)
// ============================================================================
static void __attribute__((noinline))
sparse_update_soa(PositionSoA& pos, VelocitySoA& vel, const int* indices, int idx_count, double dt) {
    for (int j = 0; j < idx_count; j++) {
        int i = indices[j];
        pos.x[i] += vel.dx[i] * dt;
        pos.y[i] += vel.dy[i] * dt;
        pos.z[i] += vel.dz[i] * dt;
    }
}

// ============================================================================
// Main
// ============================================================================
int main() {
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  Tether ECS Benchmark — 5M Entities — O3 + SoA + Prefetch ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");
    printf("Entities: %d  Iterations: %d\n\n", N, ITERS);

    // ---- Initialize SoA data ----
    auto pos_soa = PositionSoA::create(N);
    auto vel_soa = VelocitySoA::create(N);
    auto health_soa = HealthSoA::create(N);
    auto tags_soa = TagsSoA::create(N);
    pos_soa.count = N;
    vel_soa.count = N;
    health_soa.count = N;
    tags_soa.count = N;

    for (int i = 0; i < N; i++) {
        pos_soa.x[i] = (double)(i % 1000);
        pos_soa.y[i] = (double)((i / 1000) % 1000);
        pos_soa.z[i] = (double)(i % 500);
        vel_soa.dx[i] = 0.1;
        vel_soa.dy[i] = 0.2;
        vel_soa.dz[i] = 0.05;
        health_soa.hp[i] = (i % 3 == 0) ? 5.0 : 100.0;
        health_soa.max_hp[i] = 100.0;
        health_soa.alive[i] = 1;
        tags_soa.team[i] = (uint8_t)(i % 2);
        tags_soa.active[i] = true;
    }

    // Sparse indices (10% of entities)
    int sparse_count = N / 10;
    int* sparse_indices = static_cast<int*>(aligned_alloc(64, sparse_count * sizeof(int)));
    for (int j = 0; j < sparse_count; j++) {
        sparse_indices[j] = j * 10;
    }

    // ---- Initialize AoS data ----
    auto* aos_entities = static_cast<Entity*>(aligned_alloc(64, N * sizeof(Entity)));
    for (int i = 0; i < N; i++) {
        aos_entities[i].px = (double)(i % 1000);
        aos_entities[i].py = (double)((i / 1000) % 1000);
        aos_entities[i].pz = (double)(i % 500);
        aos_entities[i].dx = 0.1;
        aos_entities[i].dy = 0.2;
        aos_entities[i].dz = 0.05;
        aos_entities[i].hp = (i % 3 == 0) ? 5.0 : 100.0;
        aos_entities[i].max_hp = 100.0;
        aos_entities[i].alive = 1;
    }

    // ---- Warmup ----
    for (int w = 0; w < 3; w++) {
        movement_system_soa(pos_soa, vel_soa, 0.016);
        health_system_soa(health_soa, 0.016);
        collision_system_soa(pos_soa, health_soa);
        damage_system_soa(health_soa, 0.5);
        filter_team_active_soa(tags_soa, health_soa);
        sparse_update_soa(pos_soa, vel_soa, sparse_indices, sparse_count, 0.016);
        movement_system_aos(aos_entities, N, 0.016);
        health_system_aos(aos_entities, N, 0.016);
        collision_system_aos(aos_entities, N);
        damage_system_aos(aos_entities, N, 0.5);
    }

    double dt = 0.016;

    // ---- Benchmark 1: Movement SoA ----
    auto t0 = hrclock_t::now();
    for (int iter = 0; iter < ITERS; iter++) {
        movement_system_soa(pos_soa, vel_soa, dt);
    }
    double movement_soa_ms = ms_t(hrclock_t::now() - t0).count();

    // ---- Benchmark 1b: Movement AoS ----
    t0 = hrclock_t::now();
    for (int iter = 0; iter < ITERS; iter++) {
        movement_system_aos(aos_entities, N, dt);
    }
    double movement_aos_ms = ms_t(hrclock_t::now() - t0).count();

    // ---- Benchmark 2: Health SoA ----
    t0 = hrclock_t::now();
    for (int iter = 0; iter < ITERS; iter++) {
        health_system_soa(health_soa, dt);
    }
    double health_soa_ms = ms_t(hrclock_t::now() - t0).count();

    // ---- Benchmark 2b: Health AoS ----
    t0 = hrclock_t::now();
    for (int iter = 0; iter < ITERS; iter++) {
        health_system_aos(aos_entities, N, dt);
    }
    double health_aos_ms = ms_t(hrclock_t::now() - t0).count();

    // ---- Benchmark 3: Collision SoA ----
    t0 = hrclock_t::now();
    uint64_t total_col_soa = 0;
    for (int iter = 0; iter < ITERS; iter++) {
        total_col_soa += collision_system_soa(pos_soa, health_soa);
    }
    double collision_soa_ms = ms_t(hrclock_t::now() - t0).count();

    // ---- Benchmark 3b: Collision AoS ----
    t0 = hrclock_t::now();
    uint64_t total_col_aos = 0;
    for (int iter = 0; iter < ITERS; iter++) {
        total_col_aos += collision_system_aos(aos_entities, N);
    }
    double collision_aos_ms = ms_t(hrclock_t::now() - t0).count();

    // ---- Benchmark 4: Filter SoA ----
    t0 = hrclock_t::now();
    uint64_t total_filtered = 0;
    for (int iter = 0; iter < ITERS; iter++) {
        total_filtered += filter_team_active_soa(tags_soa, health_soa);
    }
    double filter_soa_ms = ms_t(hrclock_t::now() - t0).count();

    // ---- Benchmark 5: Damage SoA ----
    t0 = hrclock_t::now();
    for (int iter = 0; iter < ITERS; iter++) {
        damage_system_soa(health_soa, 0.5);
    }
    double damage_soa_ms = ms_t(hrclock_t::now() - t0).count();

    // ---- Benchmark 5b: Damage AoS ----
    t0 = hrclock_t::now();
    for (int iter = 0; iter < ITERS; iter++) {
        damage_system_aos(aos_entities, N, 0.5);
    }
    double damage_aos_ms = ms_t(hrclock_t::now() - t0).count();

    // ---- Benchmark 6: Sparse Update SoA ----
    t0 = hrclock_t::now();
    for (int iter = 0; iter < ITERS; iter++) {
        sparse_update_soa(pos_soa, vel_soa, sparse_indices, sparse_count, dt);
    }
    double sparse_soa_ms = ms_t(hrclock_t::now() - t0).count();

    // ---- Totals ----
    double total_soa_ms = movement_soa_ms + health_soa_ms + collision_soa_ms
                        + filter_soa_ms + damage_soa_ms + sparse_soa_ms;
    double total_aos_ms = movement_aos_ms + health_aos_ms + collision_aos_ms + damage_aos_ms;

    // ---- Results ----
    printf("┌──────────────────────────┬─────────────┬─────────────┬──────────┐\n");
    printf("│ System                   │ Tether SoA  │ Rust AoS    │ Speedup  │\n");
    printf("├──────────────────────────┼─────────────┼─────────────┼──────────┤\n");
    printf("│ Movement (5M entities)   │ %9.2f ms │ %9.2f ms │ %6.2fx  │\n", movement_soa_ms, movement_aos_ms, movement_aos_ms / movement_soa_ms);
    printf("│ Health Regen             │ %9.2f ms │ %9.2f ms │ %6.2fx  │\n", health_soa_ms, health_aos_ms, health_aos_ms / health_soa_ms);
    printf("│ Collision Detection      │ %9.2f ms │ %9.2f ms │ %6.2fx  │\n", collision_soa_ms, collision_aos_ms, collision_aos_ms / collision_soa_ms);
    printf("│ Damage Application       │ %9.2f ms │ %9.2f ms │ %6.2fx  │\n", damage_soa_ms, damage_aos_ms, damage_aos_ms / damage_soa_ms);
    printf("│ Tag Filter (branchy)     │ %9.2f ms │       n/a   │     n/a  │\n", filter_soa_ms);
    printf("│ Sparse Update (10%%)      │ %9.2f ms │       n/a   │     n/a  │\n", sparse_soa_ms);
    printf("├──────────────────────────┼─────────────┼─────────────┼──────────┤\n");
    printf("│ TOTAL                    │ %9.2f ms │ %9.2f ms │ %6.2fx  │\n", total_soa_ms, total_aos_ms, total_aos_ms / total_soa_ms);
    printf("└──────────────────────────┴─────────────┴─────────────┴──────────┘\n\n");

    // Verification
    double soa_sum = 0, aos_sum = 0;
    for (int i = 0; i < N; i++) {
        soa_sum += pos_soa.x[i];
        aos_sum += aos_entities[i].px;
    }
    printf("Verification: Tether SoA px sum = %.4f, Rust AoS px sum = %.4f\n", soa_sum, aos_sum);
    printf("Collisions: SoA = %lu, AoS = %lu\n", total_col_soa, total_col_aos);
    printf("Filter count: %lu, Sparse indices: %d\n", total_filtered, sparse_count);

    // Cleanup
    pos_soa.destroy();
    vel_soa.destroy();
    health_soa.destroy();
    tags_soa.destroy();
    free(sparse_indices);
    free(aos_entities);

    return 0;
}
