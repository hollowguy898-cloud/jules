// ============================================================================
// Tether ECS Benchmark — SoA (Struct-of-Arrays) Data-Oriented Design
//
// This C++ program represents what the Tether compiler would generate from
// Tether source code using:
//   - soa struct declarations (Struct-of-Arrays layout)
//   - align(64) cache-line alignment
//   - @simd vectorized loops
//   - explicit arena allocators
//   - defer for resource cleanup
//
// Benchmarks: 1M entities, movement + collision + rendering systems
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cmath>

using hrclock_t = std::chrono::high_resolution_clock;
using ms_t = std::chrono::duration<double, std::milli>;
using ns_t = std::chrono::duration<double, std::nano>;

// ============================================================================
// Aligned allocation (matches Tether's align(64) directive)
// ============================================================================
#define TETHER_ALIGN(x) __attribute__((aligned(x)))

// ============================================================================
// SoA Component Storage — This is what `soa struct Position { x: f64, y: f64, z: f64 }`
// generates in Tether. Each field gets its own contiguous array for maximum
// cache efficiency when iterating over a single field.
// ============================================================================
static const int NUM_ENTITIES = 1'000'000;
static const int ITERATIONS = 100;

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
        // Aligned allocation matching Tether's align(64) on arrays
        s.x = static_cast<double*>(aligned_alloc(64, cap * sizeof(double)));
        s.y = static_cast<double*>(aligned_alloc(64, cap * sizeof(double)));
        s.z = static_cast<double*>(aligned_alloc(64, cap * sizeof(double)));
        return s;
    }

    void destroy() {
        free(x);
        free(y);
        free(z);
    }
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

    void destroy() {
        free(dx);
        free(dy);
        free(dz);
    }
};

struct alignas(64) HealthSoA {
    double* hp;
    double* max_hp;
    int* alive;
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

    void destroy() {
        free(hp);
        free(max_hp);
        free(alive);
    }
};

// ============================================================================
// AoS (Array-of-Structs) — Traditional OOP layout for comparison
// This is what a naive Rust/C# ECS or regular OOP code would produce.
// ============================================================================
struct EntityAoS {
    double px, py, pz;       // position
    double dx, dy, dz;       // velocity
    double hp, max_hp;       // health
    int alive;
};

// ============================================================================
// ECS Systems
// ============================================================================

// Movement system — SoA (Tether's soa struct + @simd)
static void movement_system_soa(PositionSoA& pos, VelocitySoA& vel, double dt) {
    // @simd: LLVM will vectorize this because SoA layout = contiguous memory
    // Tether generates: !llvm.loop !{..., !"llvm.loop.vectorize.enable", i1 true}
    for (int i = 0; i < pos.count; i++) {
        pos.x[i] += vel.dx[i] * dt;
        pos.y[i] += vel.dy[i] * dt;
        pos.z[i] += vel.dz[i] * dt;
    }
}

// Movement system — AoS (traditional OOP, poor cache behavior)
static void movement_system_aos(EntityAoS* entities, int count, double dt) {
    for (int i = 0; i < count; i++) {
        entities[i].px += entities[i].dx * dt;
        entities[i].py += entities[i].dy * dt;
        entities[i].pz += entities[i].dz * dt;
    }
}

// Health regeneration system — SoA
static void health_system_soa(HealthSoA& health, double dt) {
    for (int i = 0; i < health.count; i++) {
        if (health.alive[i]) {
            health.hp[i] += 0.01 * dt;
            if (health.hp[i] > health.max_hp[i]) {
                health.hp[i] = health.max_hp[i];
            }
        }
    }
}

// Health regeneration system — AoS
static void health_system_aos(EntityAoS* entities, int count, double dt) {
    for (int i = 0; i < count; i++) {
        if (entities[i].alive) {
            entities[i].hp += 0.01 * dt;
            if (entities[i].hp > entities[i].max_hp) {
                entities[i].hp = entities[i].max_hp;
            }
        }
    }
}

// Collision detection (spatial hash) — SoA
static int collision_system_soa(PositionSoA& pos, HealthSoA& health) {
    int collisions = 0;
    double threshold = 1.0;
    // Simplified: check neighboring entities
    for (int i = 0; i < pos.count - 1; i += 2) {
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

// Collision detection — AoS
static int collision_system_aos(EntityAoS* entities, int count) {
    int collisions = 0;
    double threshold = 1.0;
    for (int i = 0; i < count - 1; i += 2) {
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
// Benchmark runner
// ============================================================================
int main() {
    printf("=== Tether ECS Benchmark (SoA vs AoS) ===\n");
    printf("Entities: %d | Iterations: %d\n\n", NUM_ENTITIES, ITERATIONS);

    // ---- Initialize SoA data ----
    auto pos_soa = PositionSoA::create(NUM_ENTITIES);
    auto vel_soa = VelocitySoA::create(NUM_ENTITIES);
    auto health_soa = HealthSoA::create(NUM_ENTITIES);
    pos_soa.count = NUM_ENTITIES;
    vel_soa.count = NUM_ENTITIES;
    health_soa.count = NUM_ENTITIES;

    for (int i = 0; i < NUM_ENTITIES; i++) {
        pos_soa.x[i] = (double)(i % 1000);
        pos_soa.y[i] = (double)((i / 1000) % 1000);
        pos_soa.z[i] = (double)(i % 500);
        vel_soa.dx[i] = 0.1;
        vel_soa.dy[i] = 0.2;
        vel_soa.dz[i] = 0.05;
        health_soa.hp[i] = 100.0;
        health_soa.max_hp[i] = 100.0;
        health_soa.alive[i] = 1;
    }

    // ---- Initialize AoS data ----
    auto* aos_entities = static_cast<EntityAoS*>(
        aligned_alloc(64, NUM_ENTITIES * sizeof(EntityAoS)));
    for (int i = 0; i < NUM_ENTITIES; i++) {
        aos_entities[i].px = (double)(i % 1000);
        aos_entities[i].py = (double)((i / 1000) % 1000);
        aos_entities[i].pz = (double)(i % 500);
        aos_entities[i].dx = 0.1;
        aos_entities[i].dy = 0.2;
        aos_entities[i].dz = 0.05;
        aos_entities[i].hp = 100.0;
        aos_entities[i].max_hp = 100.0;
        aos_entities[i].alive = 1;
    }

    // ---- Warmup ----
    for (int w = 0; w < 5; w++) {
        movement_system_soa(pos_soa, vel_soa, 0.016);
        health_system_soa(health_soa, 0.016);
        collision_system_soa(pos_soa, health_soa);
        movement_system_aos(aos_entities, NUM_ENTITIES, 0.016);
        health_system_aos(aos_entities, NUM_ENTITIES, 0.016);
        collision_system_aos(aos_entities, NUM_ENTITIES);
    }

    // ---- Benchmark SoA (Tether style) ----
    double dt = 0.016; // ~60fps
    int total_collisions_soa = 0;

    auto soa_start = hrclock_t::now();
    for (int iter = 0; iter < ITERATIONS; iter++) {
        movement_system_soa(pos_soa, vel_soa, dt);
        health_system_soa(health_soa, dt);
        total_collisions_soa += collision_system_soa(pos_soa, health_soa);
    }
    auto soa_end = hrclock_t::now();
    double soa_ms = ms_t(soa_end - soa_start).count();

    // ---- Benchmark AoS (Traditional OOP) ----
    int total_collisions_aos = 0;

    auto aos_start = hrclock_t::now();
    for (int iter = 0; iter < ITERATIONS; iter++) {
        movement_system_aos(aos_entities, NUM_ENTITIES, dt);
        health_system_aos(aos_entities, NUM_ENTITIES, dt);
        total_collisions_aos += collision_system_aos(aos_entities, NUM_ENTITIES);
    }
    auto aos_end = hrclock_t::now();
    double aos_ms = ms_t(aos_end - aos_start).count();

    // ---- Results ----
    printf("+-------------------------------------------+\n");
    printf("| TETHER ECS BENCHMARK RESULTS              |\n");
    printf("+-------------------------------------------+\n");
    printf("| Layout  | Time (ms) | Throughput (Ment/s) |\n");
    printf("|---------|-----------|---------------------|\n");
    printf("| SoA *   | %9.2f | %19.2f |\n", soa_ms, (NUM_ENTITIES / 1e6) * ITERATIONS / (soa_ms / 1000.0));
    printf("| AoS     | %9.2f | %19.2f |\n", aos_ms, (NUM_ENTITIES / 1e6) * ITERATIONS / (aos_ms / 1000.0));
    printf("+-------------------------------------------+\n");
    printf("| * Tether soa struct + align(64) + @simd   |\n");
    printf("+-------------------------------------------+\n\n");
    printf("SoA speedup: %.2fx faster than AoS\n", aos_ms / soa_ms);
    printf("SoA per-iteration: %.3f ms | AoS per-iteration: %.3f ms\n",
           soa_ms / ITERATIONS, aos_ms / ITERATIONS);

    // Verify results
    double soa_sum = 0, aos_sum = 0;
    for (int i = 0; i < NUM_ENTITIES; i++) {
        soa_sum += pos_soa.x[i];
        aos_sum += aos_entities[i].px;
    }
    printf("\nVerification: SoA sum=%.4f, AoS sum=%.4f\n", soa_sum, aos_sum);

    // Cleanup (Tether's defer would handle this)
    pos_soa.destroy();
    vel_soa.destroy();
    health_soa.destroy();
    free(aos_entities);

    return 0;
}
