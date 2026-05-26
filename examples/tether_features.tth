// Tether Language Feature Showcase
// Demonstrates @simd, @polly, @superoptimize, soa struct, and explicit allocators

// SoA struct: cache-friendly for bulk processing
soa struct Particle {
    x: f64,
    y: f64,
    z: f64,
    vx: f64,
    vy: f64,
    vz: f64,
    mass: f64,
    active: u8,
}

// Regular struct for padding warning demo
struct BadLayout {
    active: u8,
    pointer: *u64,
    flag: u8,
}

// Well-packed struct (no warning expected)
struct GoodLayout {
    pointer: *u64,
    active: u8,
    flag: u8,
}

@simd fn sum_vector(data: []f64, len: u64) f64 {
    var total: f64 = 0.0;
    var i: u64 = 0;
    while (i < len) {
        total = total + data[i];
        i += 1;
    }
    return total;
}

@superoptimize fn fast_fib(n: i32) i32 {
    if (n <= 1) {
        return n;
    }
    var a: i32 = 0;
    var b: i32 = 1;
    var i: i32 = 2;
    while (i <= n) {
        var temp: i32 = b;
        b = a + b;
        a = temp;
        i += 1;
    }
    return b;
}

fn main() i32 {
    val result: i32 = fast_fib(10);
    return result;
}
