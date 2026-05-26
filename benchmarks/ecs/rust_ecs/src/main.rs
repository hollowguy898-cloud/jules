// ============================================================================
// Rust ECS Benchmark — AoS (Array-of-Structs) Data Layout
//
// This is a standard Rust ECS-style program using AoS layout, which is the
// default for Rust structs. We compare against Tether's SoA approach.
//
// Benchmarks: 1M entities, movement + collision + rendering systems
// ============================================================================

use std::time::Instant;

const NUM_ENTITIES: usize = 1_000_000;
const ITERATIONS: i32 = 100;

// ============================================================================
// AoS Entity struct (Rust default — fields are interleaved in memory)
// ============================================================================
#[derive(Clone)]
struct Entity {
    px: f64, py: f64, pz: f64,     // position
    dx: f64, dy: f64, dz: f64,     // velocity
    hp: f64, max_hp: f64,           // health
    alive: bool,
}

// ============================================================================
// SoA Component Storage (manual implementation in Rust)
// ============================================================================
struct PositionSoA {
    x: Vec<f64>,
    y: Vec<f64>,
    z: Vec<f64>,
}

struct VelocitySoA {
    dx: Vec<f64>,
    dy: Vec<f64>,
    dz: Vec<f64>,
}

struct HealthSoA {
    hp: Vec<f64>,
    max_hp: Vec<f64>,
    alive: Vec<bool>,
}

// ============================================================================
// Movement system — SoA
// ============================================================================
fn movement_system_soa(pos: &mut PositionSoA, vel: &VelocitySoA, dt: f64) {
    let n = pos.x.len();
    for i in 0..n {
        pos.x[i] += vel.dx[i] * dt;
        pos.y[i] += vel.dy[i] * dt;
        pos.z[i] += vel.dz[i] * dt;
    }
}

// ============================================================================
// Movement system — AoS
// ============================================================================
fn movement_system_aos(entities: &mut [Entity], dt: f64) {
    for e in entities.iter_mut() {
        e.px += e.dx * dt;
        e.py += e.dy * dt;
        e.pz += e.dz * dt;
    }
}

// ============================================================================
// Health system — SoA
// ============================================================================
fn health_system_soa(health: &mut HealthSoA, dt: f64) {
    let n = health.hp.len();
    for i in 0..n {
        if health.alive[i] {
            health.hp[i] += 0.01 * dt;
            if health.hp[i] > health.max_hp[i] {
                health.hp[i] = health.max_hp[i];
            }
        }
    }
}

// ============================================================================
// Health system — AoS
// ============================================================================
fn health_system_aos(entities: &mut [Entity], dt: f64) {
    for e in entities.iter_mut() {
        if e.alive {
            e.hp += 0.01 * dt;
            if e.hp > e.max_hp {
                e.hp = e.max_hp;
            }
        }
    }
}

// ============================================================================
// Collision system — SoA
// ============================================================================
fn collision_system_soa(pos: &PositionSoA, health: &mut HealthSoA) -> i32 {
    let mut collisions = 0;
    let threshold = 1.0_f64;
    let n = pos.x.len();
    let mut i = 0;
    while i + 1 < n {
        let dx = pos.x[i] - pos.x[i + 1];
        let dy = pos.y[i] - pos.y[i + 1];
        let dz = pos.z[i] - pos.z[i + 1];
        let dist_sq = dx * dx + dy * dy + dz * dz;
        if dist_sq < threshold && health.alive[i] && health.alive[i + 1] {
            health.hp[i] -= 0.1;
            health.hp[i + 1] -= 0.1;
            collisions += 1;
        }
        i += 2;
    }
    collisions
}

// ============================================================================
// Collision system — AoS
// ============================================================================
fn collision_system_aos(entities: &mut [Entity]) -> i32 {
    let mut collisions = 0;
    let threshold = 1.0_f64;
    let n = entities.len();
    let mut i = 0;
    while i + 1 < n {
        let dx = entities[i].px - entities[i + 1].px;
        let dy = entities[i].py - entities[i + 1].py;
        let dz = entities[i].pz - entities[i + 1].pz;
        let dist_sq = dx * dx + dy * dy + dz * dz;
        if dist_sq < threshold && entities[i].alive && entities[i + 1].alive {
            entities[i].hp -= 0.1;
            entities[i + 1].hp -= 0.1;
            collisions += 1;
        }
        i += 2;
    }
    collisions
}

fn main() {
    println!("=== Rust ECS Benchmark (SoA vs AoS) ===");
    println!("Entities: {} | Iterations: {}\n", NUM_ENTITIES, ITERATIONS);

    // ---- Initialize SoA data ----
    let mut pos_soa = PositionSoA {
        x: (0..NUM_ENTITIES).map(|i| (i % 1000) as f64).collect(),
        y: (0..NUM_ENTITIES).map(|i| ((i / 1000) % 1000) as f64).collect(),
        z: (0..NUM_ENTITIES).map(|i| (i % 500) as f64).collect(),
    };
    let vel_soa = VelocitySoA {
        dx: vec![0.1; NUM_ENTITIES],
        dy: vec![0.2; NUM_ENTITIES],
        dz: vec![0.05; NUM_ENTITIES],
    };
    let mut health_soa = HealthSoA {
        hp: vec![100.0; NUM_ENTITIES],
        max_hp: vec![100.0; NUM_ENTITIES],
        alive: vec![true; NUM_ENTITIES],
    };

    // ---- Initialize AoS data ----
    let mut aos_entities: Vec<Entity> = (0..NUM_ENTITIES)
        .map(|i| Entity {
            px: (i % 1000) as f64,
            py: ((i / 1000) % 1000) as f64,
            pz: (i % 500) as f64,
            dx: 0.1,
            dy: 0.2,
            dz: 0.05,
            hp: 100.0,
            max_hp: 100.0,
            alive: true,
        })
        .collect();

    // ---- Warmup ----
    for _ in 0..5 {
        movement_system_soa(&mut pos_soa, &vel_soa, 0.016);
        health_system_soa(&mut health_soa, 0.016);
        collision_system_soa(&pos_soa, &mut health_soa);
        movement_system_aos(&mut aos_entities, 0.016);
        health_system_aos(&mut aos_entities, 0.016);
        collision_system_aos(&mut aos_entities);
    }

    let dt = 0.016_f64;

    // ---- Benchmark SoA ----
    let mut total_collisions_soa = 0;
    let soa_start = Instant::now();
    for _ in 0..ITERATIONS {
        movement_system_soa(&mut pos_soa, &vel_soa, dt);
        health_system_soa(&mut health_soa, dt);
        total_collisions_soa += collision_system_soa(&pos_soa, &mut health_soa);
    }
    let soa_elapsed = soa_start.elapsed().as_millis() as f64;

    // ---- Benchmark AoS ----
    let mut total_collisions_aos = 0;
    let aos_start = Instant::now();
    for _ in 0..ITERATIONS {
        movement_system_aos(&mut aos_entities, dt);
        health_system_aos(&mut aos_entities, dt);
        total_collisions_aos += collision_system_aos(&mut aos_entities);
    }
    let aos_elapsed = aos_start.elapsed().as_millis() as f64;

    // ---- Results ----
    let soa_throughput = (NUM_ENTITIES as f64 / 1e6) * ITERATIONS as f64 / (soa_elapsed / 1000.0);
    let aos_throughput = (NUM_ENTITIES as f64 / 1e6) * ITERATIONS as f64 / (aos_elapsed / 1000.0);

    println!("+-------------------------------------------+");
    println!("| RUST ECS BENCHMARK RESULTS                |");
    println!("+-------------------------------------------+");
    println!("| Layout  | Time (ms) | Throughput (Ment/s) |");
    println!("|---------|-----------|---------------------|");
    println!("| SoA     | {:9.2} | {:19.2} |", soa_elapsed, soa_throughput);
    println!("| AoS     | {:9.2} | {:19.2} |", aos_elapsed, aos_throughput);
    println!("+-------------------------------------------+");
    println!();
    println!("SoA speedup: {:.2}x faster than AoS", aos_elapsed / soa_elapsed);
    println!("SoA per-iteration: {:.3} ms | AoS per-iteration: {:.3} ms",
             soa_elapsed / ITERATIONS as f64, aos_elapsed / ITERATIONS as f64);

    // Verify
    let soa_sum: f64 = pos_soa.x.iter().sum();
    let aos_sum: f64 = aos_entities.iter().map(|e| e.px).sum();
    println!("\nVerification: SoA sum={:.4}, AoS sum={:.4}", soa_sum, aos_sum);
}
