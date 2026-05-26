// ============================================================================
// Rust ECS Benchmark — 5M Entities — SoA Layout — O3
//
// This is the Rust SoA version (manually written, since Rust has no `soa struct`).
// Compare against Tether's auto-SoA output.
//
// Compile: rustc -O3 -C target-cpu=native rust_ecs_5m_soa.rs -o rust_ecs_5m_soa
// Run:     ./rust_ecs_5m_soa
// ============================================================================

use std::time::Instant;

const N: usize = 5_000_000;
const ITERS: usize = 50;

// ============================================================================
// SoA Component Storage (manual in Rust, automatic in Tether)
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

struct TagsSoA {
    team: Vec<u8>,
    active: Vec<bool>,
}

// ============================================================================
// System 1: Movement — SoA
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
// System 2: Health Regen — SoA
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
// System 3: Collision — SoA
// ============================================================================
fn collision_system_soa(pos: &PositionSoA, health: &mut HealthSoA) -> u64 {
    let mut collisions: u64 = 0;
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
// System 4: Tag Filter — SoA
// ============================================================================
fn filter_team_active_soa(tags: &TagsSoA, health: &HealthSoA) -> u64 {
    let mut count: u64 = 0;
    let n = tags.team.len();
    for i in 0..n {
        if tags.team[i] == 1 && tags.active[i] && health.alive[i] && health.hp[i] > 10.0 {
            count += 1;
        }
    }
    count
}

// ============================================================================
// System 5: Damage — SoA
// ============================================================================
fn damage_system_soa(health: &mut HealthSoA, dmg: f64) {
    let n = health.hp.len();
    for i in 0..n {
        if health.alive[i] {
            health.hp[i] -= dmg;
        }
    }
}

// ============================================================================
// System 6: Sparse Update — SoA
// ============================================================================
fn sparse_update_soa(pos: &mut PositionSoA, vel: &VelocitySoA, indices: &[usize], dt: f64) {
    for &idx in indices {
        pos.x[idx] += vel.dx[idx] * dt;
        pos.y[idx] += vel.dy[idx] * dt;
        pos.z[idx] += vel.dz[idx] * dt;
    }
}

fn main() {
    println!("╔══════════════════════════════════════════════════════════════╗");
    println!("║   Rust ECS Benchmark — 5M Entities — SoA — O3 (-C opt=3)  ║");
    println!("╚══════════════════════════════════════════════════════════════╝");
    println!("Entities: {}  Iterations: {}", N, ITERS);
    println!();

    // ---- Initialize SoA data ----
    let mut pos_soa = PositionSoA {
        x: (0..N).map(|i| (i % 1000) as f64).collect(),
        y: (0..N).map(|i| ((i / 1000) % 1000) as f64).collect(),
        z: (0..N).map(|i| (i % 500) as f64).collect(),
    };
    let vel_soa = VelocitySoA {
        dx: vec![0.1; N],
        dy: vec![0.2; N],
        dz: vec![0.05; N],
    };
    let mut health_soa = HealthSoA {
        hp: (0..N).map(|i| if i % 3 == 0 { 5.0 } else { 100.0 }).collect(),
        max_hp: vec![100.0; N],
        alive: vec![true; N],
    };
    let tags_soa = TagsSoA {
        team: (0..N).map(|i| if i % 2 == 0 { 0u8 } else { 1u8 }).collect(),
        active: vec![true; N],
    };
    let sparse_indices: Vec<usize> = (0..N).step_by(10).collect();

    // ---- Warmup ----
    for _ in 0..3 {
        movement_system_soa(&mut pos_soa, &vel_soa, 0.016);
        health_system_soa(&mut health_soa, 0.016);
        collision_system_soa(&pos_soa, &mut health_soa);
        damage_system_soa(&mut health_soa, 0.5);
        filter_team_active_soa(&tags_soa, &health_soa);
        sparse_update_soa(&mut pos_soa, &vel_soa, &sparse_indices, 0.016);
    }

    let dt = 0.016_f64;

    // ---- Benchmark 1: Movement SoA ----
    let start = Instant::now();
    for _ in 0..ITERS {
        movement_system_soa(&mut pos_soa, &vel_soa, dt);
    }
    let movement_soa_ms = start.elapsed().as_millis() as f64;

    // ---- Benchmark 2: Health SoA ----
    let start = Instant::now();
    for _ in 0..ITERS {
        health_system_soa(&mut health_soa, dt);
    }
    let health_soa_ms = start.elapsed().as_millis() as f64;

    // ---- Benchmark 3: Collision SoA ----
    let start = Instant::now();
    let mut total_col = 0u64;
    for _ in 0..ITERS {
        total_col += collision_system_soa(&pos_soa, &mut health_soa);
    }
    let collision_soa_ms = start.elapsed().as_millis() as f64;

    // ---- Benchmark 4: Filter SoA ----
    let start = Instant::now();
    let mut total_filtered = 0u64;
    for _ in 0..ITERS {
        total_filtered += filter_team_active_soa(&tags_soa, &health_soa);
    }
    let filter_soa_ms = start.elapsed().as_millis() as f64;

    // ---- Benchmark 5: Damage SoA ----
    let start = Instant::now();
    for _ in 0..ITERS {
        damage_system_soa(&mut health_soa, 0.5);
    }
    let damage_soa_ms = start.elapsed().as_millis() as f64;

    // ---- Benchmark 6: Sparse Update SoA ----
    let start = Instant::now();
    for _ in 0..ITERS {
        sparse_update_soa(&mut pos_soa, &vel_soa, &sparse_indices, dt);
    }
    let sparse_soa_ms = start.elapsed().as_millis() as f64;

    // ---- Total ----
    let total_soa_ms = movement_soa_ms + health_soa_ms + collision_soa_ms
                     + filter_soa_ms + damage_soa_ms + sparse_soa_ms;

    // ---- Output results in parseable format ----
    println!("RUST_SOA_RESULTS_START");
    println!("movement_soa_ms:{}", movement_soa_ms);
    println!("health_soa_ms:{}", health_soa_ms);
    println!("collision_soa_ms:{}", collision_soa_ms);
    println!("filter_soa_ms:{}", filter_soa_ms);
    println!("damage_soa_ms:{}", damage_soa_ms);
    println!("sparse_soa_ms:{}", sparse_soa_ms);
    println!("total_soa_ms:{}", total_soa_ms);
    println!("RUST_SOA_RESULTS_END");
    println!();

    println!("┌──────────────────────────┬─────────────┐");
    println!("│ System (Rust SoA)        │ Time (ms)   │");
    println!("├──────────────────────────┼─────────────┤");
    println!("│ Movement (5M entities)   │ {:9.2} ms │", movement_soa_ms);
    println!("│ Health Regen             │ {:9.2} ms │", health_soa_ms);
    println!("│ Collision Detection      │ {:9.2} ms │", collision_soa_ms);
    println!("│ Tag Filter (branchy)     │ {:9.2} ms │", filter_soa_ms);
    println!("│ Damage Application       │ {:9.2} ms │", damage_soa_ms);
    println!("│ Sparse Update (10%)      │ {:9.2} ms │", sparse_soa_ms);
    println!("├──────────────────────────┼─────────────┤");
    println!("│ TOTAL                    │ {:9.2} ms │", total_soa_ms);
    println!("└──────────────────────────┴─────────────┘");

    // Verification
    let soa_sum: f64 = pos_soa.x.iter().sum();
    println!("\nVerification: SoA px sum = {:.4}", soa_sum);
    println!("Collision count: {}, Filter count: {}", total_col, total_filtered);
}
