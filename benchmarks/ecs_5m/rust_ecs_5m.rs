// ============================================================================
// Rust ECS Benchmark — 5 Million Entities — O3 Optimization
//
// Comprehensive ECS workload comparing AoS (Rust default) vs SoA
// (what Tether auto-generates with `soa struct`).
//
// Compile: rustc -O3 rust_ecs_5m.rs -o rust_ecs_5m
// Run:     ./rust_ecs_5m
// ============================================================================

use std::time::Instant;

const N: usize = 5_000_000;
const ITERS: usize = 50;

// ============================================================================
// AoS Entity struct (Rust default — fields interleaved in memory)
// ============================================================================
#[derive(Clone)]
struct Entity {
    px: f64, py: f64, pz: f64,     // position
    dx: f64, dy: f64, dz: f64,     // velocity
    hp: f64, max_hp: f64,           // health
    alive: bool,
}

// ============================================================================
// SoA Component Storage (manual in Rust, auto in Tether)
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
    team: Vec<u8>,      // 0 or 1
    active: Vec<bool>,
}

// ============================================================================
// Benchmark 1: Movement System — SoA (cache-friendly stride-1 access)
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
// Benchmark 1b: Movement System — AoS (stride-8 access, poor cache)
// ============================================================================
fn movement_system_aos(entities: &mut [Entity], dt: f64) {
    for e in entities.iter_mut() {
        e.px += e.dx * dt;
        e.py += e.dy * dt;
        e.pz += e.dz * dt;
    }
}

// ============================================================================
// Benchmark 2: Health Regen System — SoA
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
// Benchmark 2b: Health Regen System — AoS
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
// Benchmark 3: Collision System — SoA (pairwise neighbor check)
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
// Benchmark 3b: Collision System — AoS
// ============================================================================
fn collision_system_aos(entities: &mut [Entity]) -> u64 {
    let mut collisions: u64 = 0;
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

// ============================================================================
// Benchmark 4: Tag Filter — SoA (branch-heavy scan)
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
// Benchmark 5: Damage Application — SoA (straight-line, vectorizable)
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
// Benchmark 5b: Damage Application — AoS
// ============================================================================
fn damage_system_aos(entities: &mut [Entity], dmg: f64) {
    for e in entities.iter_mut() {
        if e.alive {
            e.hp -= dmg;
        }
    }
}

// ============================================================================
// Benchmark 6: Sparse Update — SoA (10% active subset)
// ============================================================================
fn sparse_update_soa(pos: &mut PositionSoA, vel: &VelocitySoA, indices: &[usize], dt: f64) {
    for &idx in indices {
        pos.x[idx] += vel.dx[idx] * dt;
        pos.y[idx] += vel.dy[idx] * dt;
        pos.z[idx] += vel.dz[idx] * dt;
    }
}

fn main() {
    println!("╔══════════════════════════════════════════════════════════╗");
    println!("║   Rust ECS Benchmark — 5M Entities — O3 (-C opt-level=3) ║");
    println!("╚══════════════════════════════════════════════════════════╝");
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
        hp: vec![100.0; N],
        max_hp: vec![100.0; N],
        alive: vec![true; N],
    };
    // Damage some entities
    for i in (0..N).step_by(3) {
        health_soa.hp[i] = 5.0;
    }
    let tags_soa = TagsSoA {
        team: (0..N).map(|i| if i % 2 == 0 { 0u8 } else { 1u8 }).collect(),
        active: vec![true; N],
    };
    // Sparse indices (10% of entities)
    let sparse_indices: Vec<usize> = (0..N).step_by(10).collect();

    // ---- Initialize AoS data ----
    let mut aos_entities: Vec<Entity> = (0..N)
        .map(|i| Entity {
            px: (i % 1000) as f64,
            py: ((i / 1000) % 1000) as f64,
            pz: (i % 500) as f64,
            dx: 0.1,
            dy: 0.2,
            dz: 0.05,
            hp: if i % 3 == 0 { 5.0 } else { 100.0 },
            max_hp: 100.0,
            alive: true,
        })
        .collect();

    // ---- Warmup ----
    for _ in 0..3 {
        movement_system_soa(&mut pos_soa, &vel_soa, 0.016);
        health_system_soa(&mut health_soa, 0.016);
        collision_system_soa(&pos_soa, &mut health_soa);
        damage_system_soa(&mut health_soa, 0.5);
        filter_team_active_soa(&tags_soa, &health_soa);
        sparse_update_soa(&mut pos_soa, &vel_soa, &sparse_indices, 0.016);

        movement_system_aos(&mut aos_entities, 0.016);
        health_system_aos(&mut aos_entities, 0.016);
        collision_system_aos(&mut aos_entities);
        damage_system_aos(&mut aos_entities, 0.5);
    }

    let dt = 0.016_f64;

    // ---- Benchmark 1: Movement SoA ----
    let start = Instant::now();
    for _ in 0..ITERS {
        movement_system_soa(&mut pos_soa, &vel_soa, dt);
    }
    let movement_soa_ms = start.elapsed().as_millis() as f64;

    // ---- Benchmark 1b: Movement AoS ----
    let start = Instant::now();
    for _ in 0..ITERS {
        movement_system_aos(&mut aos_entities, dt);
    }
    let movement_aos_ms = start.elapsed().as_millis() as f64;

    // ---- Benchmark 2: Health SoA ----
    let start = Instant::now();
    for _ in 0..ITERS {
        health_system_soa(&mut health_soa, dt);
    }
    let health_soa_ms = start.elapsed().as_millis() as f64;

    // ---- Benchmark 2b: Health AoS ----
    let start = Instant::now();
    for _ in 0..ITERS {
        health_system_aos(&mut aos_entities, dt);
    }
    let health_aos_ms = start.elapsed().as_millis() as f64;

    // ---- Benchmark 3: Collision SoA ----
    let start = Instant::now();
    let mut total_col = 0u64;
    for _ in 0..ITERS {
        total_col += collision_system_soa(&pos_soa, &mut health_soa);
    }
    let collision_soa_ms = start.elapsed().as_millis() as f64;

    // ---- Benchmark 3b: Collision AoS ----
    let start = Instant::now();
    let mut total_col_aos = 0u64;
    for _ in 0..ITERS {
        total_col_aos += collision_system_aos(&mut aos_entities);
    }
    let collision_aos_ms = start.elapsed().as_millis() as f64;

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

    // ---- Benchmark 5b: Damage AoS ----
    let start = Instant::now();
    for _ in 0..ITERS {
        damage_system_aos(&mut aos_entities, 0.5);
    }
    let damage_aos_ms = start.elapsed().as_millis() as f64;

    // ---- Benchmark 6: Sparse Update SoA ----
    let start = Instant::now();
    for _ in 0..ITERS {
        sparse_update_soa(&mut pos_soa, &vel_soa, &sparse_indices, dt);
    }
    let sparse_soa_ms = start.elapsed().as_millis() as f64;

    // ---- Totals ----
    let total_soa_ms = movement_soa_ms + health_soa_ms + collision_soa_ms
                     + filter_soa_ms + damage_soa_ms + sparse_soa_ms;
    let total_aos_ms = movement_aos_ms + health_aos_ms + collision_aos_ms
                     + damage_aos_ms;

    // ---- Results ----
    println!("┌──────────────────────────┬────────────┬────────────┬──────────┐");
    println!("│ System                   │ Rust SoA   │ Rust AoS   │ Speedup  │");
    println!("├──────────────────────────┼────────────┼────────────┼──────────┤");
    println!("│ Movement (5M entities)   │ {:8.2} ms │ {:8.2} ms │ {:6.2}x  │", movement_soa_ms, movement_aos_ms, movement_aos_ms / movement_soa_ms);
    println!("│ Health Regen             │ {:8.2} ms │ {:8.2} ms │ {:6.2}x  │", health_soa_ms, health_aos_ms, health_aos_ms / health_soa_ms);
    println!("│ Collision Detection      │ {:8.2} ms │ {:8.2} ms │ {:6.2}x  │", collision_soa_ms, collision_aos_ms, collision_aos_ms / collision_soa_ms);
    println!("│ Damage Application       │ {:8.2} ms │ {:8.2} ms │ {:6.2}x  │", damage_soa_ms, damage_aos_ms, damage_aos_ms / damage_soa_ms);
    println!("│ Tag Filter (branchy)     │ {:8.2} ms │       n/a  │     n/a  │", filter_soa_ms);
    println!("│ Sparse Update (10%)      │ {:8.2} ms │       n/a  │     n/a  │", sparse_soa_ms);
    println!("├──────────────────────────┼────────────┼────────────┼──────────┤");
    println!("│ TOTAL                    │ {:8.2} ms │ {:8.2} ms │ {:6.2}x  │", total_soa_ms, total_aos_ms, total_aos_ms / total_soa_ms);
    println!("└──────────────────────────┴────────────┴────────────┴──────────┘");
    println!();

    // Verification
    let soa_sum: f64 = pos_soa.x.iter().sum();
    let aos_sum: f64 = aos_entities.iter().map(|e| e.px).sum();
    println!("Verification: SoA px sum = {:.4}, AoS px sum = {:.4}", soa_sum, aos_sum);
    println!("Collision count: SoA = {}, AoS = {}", total_col, total_col_aos);
    println!("Filter count: {}", total_filtered);
    println!("Sparse indices: {}", sparse_indices.len());
}
