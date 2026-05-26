// Tether ECS Demo - Showcasing new language features
// SoA struct with cache-line alignment
soa struct Position {
    x: f64,
    y: f64,
    z: f64
}

soa struct Velocity {
    dx: f64,
    dy: f64,
    dz: f64
}

struct Health {
    hp: f64,
    max_hp: f64,
    alive: bool
}

// Error type for ECS operations
enum EcsError {
    EntityNotFound = 0,
    ComponentMissing = 1,
    InvalidState = 2
}

// Movement system using @simd
@simd
fn movement_system(x: f64, y: f64, z: f64, dx: f64, dy: f64, dz: f64, dt: f64) f64 {
    val nx: f64 = x + dx * dt;
    return nx;
}

// Health regen with error handling
fn regenerate_health(hp: f64, max_hp: f64, alive: bool) !f64 {
    if (!alive) {
        return 2;
    }
    val new_hp: f64 = hp + 0.01;
    if (new_hp > max_hp) {
        return max_hp;
    }
    return new_hp;
}

// Physics with try error propagation
fn physics_step(x: f64, dx: f64, dt: f64, hp: f64, alive: bool) !f64 {
    val new_x: f64 = movement_system(x, 0.0, 0.0, dx, 0.0, 0.0, dt);
    val new_hp: f64 = try regenerate_health(hp, 100.0, alive);
    return new_x;
}

// Main entry point demonstrating features
fn main() i64 {
    val pos_x: f64 = 100.0;
    val vel_dx: f64 = 0.5;
    val dt: f64 = 0.016;
    val hp: f64 = 95.0;
    val alive: bool = true;

    // Run movement with SIMD
    val result: f64 = movement_system(pos_x, 0.0, 0.0, vel_dx, 0.0, 0.0, dt);

    // Try error propagation
    val hp_result: f64 = try regenerate_health(hp, 100.0, alive);

    // Atomic counter increment
    var counter: i64 = 0;
    atomic counter = counter + 1;

    // Yield to scheduler
    yield;

    // Defer cleanup
    defer counter = 0;

    return 0;
}
