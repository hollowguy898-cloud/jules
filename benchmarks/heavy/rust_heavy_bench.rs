// ============================================================================
// Rust Heavy Workload Benchmark Suite vs Tether
// Scaled to match Tether's benchmark sizes.
//
// Compile: rustc -O -C target-cpu=native -C opt-level=3 rust_heavy_bench.rs -o rust_heavy_bench
// ============================================================================

use std::time::Instant;
use std::alloc::{alloc, dealloc, Layout};
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::Arc;
use std::thread;

macro_rules! bench_start { ($name:expr) => { println!("BENCH_{}_START", $name); } }
macro_rules! bench_end { ($name:expr) => { println!("BENCH_{}_END", $name); } }
macro_rules! bench_result { ($key:expr, $val:expr) => { println!("  {}:{:.4}", $key, $val); } }

// ============================================================================
// BENCHMARK 1: 20M Entity ECS — 10 Systems (AoS, Rust default layout)
// ============================================================================

const ECS_N: usize = 20_000_000;
const ECS_ITERS: usize = 30;

#[repr(C)]
#[derive(Clone)]
struct Entity {
    px: f64, py: f64, pz: f64,
    dx: f64, dy: f64, dz: f64,
    hp: f64, max_hp: f64,
    mass: f64, inv_mass: f64,
    ai_state: i32, ai_timer: f64,
    alive: i32,
    team: u8, active: u8, damageable: u8, visible: u8,
}

#[inline(never)]
fn ecs_move(e: &mut [Entity], dt: f64) {
    for i in 0..e.len() {
        e[i].px += e[i].dx * dt;
        e[i].py += e[i].dy * dt;
        e[i].pz += e[i].dz * dt;
    }
}

#[inline(never)]
fn ecs_regen(e: &mut [Entity], dt: f64) {
    for i in 0..e.len() {
        if e[i].alive != 0 {
            e[i].hp += 0.01 * dt;
            if e[i].hp > e[i].max_hp { e[i].hp = e[i].max_hp; }
        }
    }
}

#[inline(never)]
fn ecs_col(e: &mut [Entity]) -> u64 {
    let mut c: u64 = 0;
    let th = 2.0_f64;
    let n = e.len();
    let mut i = 0;
    while i + 8 < n {
        let dx = e[i].px - e[i+1].px;
        let dy = e[i].py - e[i+1].py;
        let dz = e[i].pz - e[i+1].pz;
        if dx*dx + dy*dy + dz*dz < th {
            if e[i].damageable != 0 && e[i+1].damageable != 0 {
                e[i].hp -= 0.5; e[i+1].hp -= 0.5; c += 1;
            }
        }
        i += 8;
    }
    c
}

#[inline(never)]
fn ecs_dmg(e: &mut [Entity], dmg: f64) {
    for ent in e.iter_mut() {
        if ent.damageable != 0 && ent.alive != 0 { ent.hp -= dmg; }
    }
}

#[inline(never)]
fn ecs_filt(e: &[Entity]) -> u64 {
    let mut c: u64 = 0;
    for ent in e.iter() {
        if ent.team == 1 && ent.active != 0 && ent.visible != 0 && ent.alive != 0 && ent.hp > 10.0 { c += 1; }
    }
    c
}

#[inline(never)]
fn ecs_sparse(e: &mut [Entity], idx: &[usize], dt: f64) {
    for &i in idx {
        e[i].px += e[i].dx * dt * 2.0;
        e[i].py += e[i].dy * dt * 2.0;
        e[i].pz += e[i].dz * dt * 2.0;
    }
}

#[inline(never)]
fn ecs_ai(e: &mut [Entity], dt: f64) {
    for ent in e.iter_mut() {
        if ent.alive == 0 { continue; }
        ent.ai_timer -= dt;
        if ent.ai_timer <= 0.0 {
            ent.ai_state = (ent.ai_state + 1) % 3;
            ent.ai_timer = 1.0 + ((ent.ai_state as i64) % 5) as f64 * 0.5;
        }
    }
}

#[inline(never)]
fn ecs_phys(e: &mut [Entity], dt: f64) {
    let g = -9.81_f64;
    for ent in e.iter_mut() {
        ent.dz += g * ent.mass * ent.inv_mass * dt;
        ent.pz += ent.dz * dt;
        if ent.pz < 0.0 { ent.pz = 0.0; ent.dz *= -0.5; }
    }
}

#[inline(never)]
fn ecs_death(e: &mut [Entity]) -> u64 {
    let mut d: u64 = 0;
    for ent in e.iter_mut() {
        if ent.alive != 0 && ent.hp <= 0.0 { ent.alive = 0; ent.active = 0; d += 1; }
    }
    d
}

#[inline(never)]
fn ecs_part(e: &mut [Entity], dt: f64) {
    for ent in e.iter_mut() {
        if ent.visible == 0 { continue; }
        ent.dx *= 0.99; ent.dy *= 0.99; ent.dz *= 0.99;
        ent.px += ent.dx * dt; ent.py += ent.dy * dt;
    }
}

fn bench_ecs() {
    bench_start!("ECS_20M");
    println!("  Init 20M entities, 10 systems, {} frames...", ECS_ITERS);

    let mut entities: Vec<Entity> = (0..ECS_N).map(|i| Entity {
        px: (i % 2000) as f64, py: ((i / 2000) % 2000) as f64, pz: (i % 500) as f64,
        dx: 0.1 + (i % 10) as f64 * 0.01, dy: 0.2 - (i % 7) as f64 * 0.01, dz: 0.05,
        hp: if i % 3 == 0 { 5.0 } else { 100.0 }, max_hp: 100.0,
        mass: 1.0 + (i % 10) as f64 * 0.5, inv_mass: 1.0 / (1.0 + (i % 10) as f64 * 0.5),
        ai_state: (i % 4) as i32, ai_timer: 1.0 + (i % 5) as f64 * 0.5,
        alive: 1, team: (i % 2) as u8, active: 1, damageable: if i % 5 != 0 { 1 } else { 0 }, visible: if i % 3 != 0 { 1 } else { 0 },
    }).collect();

    let sparse: Vec<usize> = (0..ECS_N).step_by(20).collect();

    for _ in 0..2 {
        ecs_move(&mut entities, 0.016); ecs_regen(&mut entities, 0.016);
        ecs_col(&mut entities); ecs_dmg(&mut entities, 0.5);
        ecs_filt(&entities); ecs_sparse(&mut entities, &sparse, 0.016);
        ecs_ai(&mut entities, 0.016); ecs_phys(&mut entities, 0.016);
        ecs_death(&mut entities); ecs_part(&mut entities, 0.016);
    }

    for (i, e) in entities.iter_mut().enumerate() {
        e.hp = if i % 3 == 0 { 5.0 } else { 100.0 }; e.alive = 1; e.active = 1;
    }

    let mut tc: u64 = 0; let mut tf: u64 = 0; let mut td: u64 = 0;
    let start = Instant::now();
    for _ in 0..ECS_ITERS {
        ecs_move(&mut entities, 0.016); ecs_regen(&mut entities, 0.016);
        tc += ecs_col(&mut entities); ecs_dmg(&mut entities, 0.5);
        tf += ecs_filt(&entities); ecs_sparse(&mut entities, &sparse, 0.016);
        ecs_ai(&mut entities, 0.016); ecs_phys(&mut entities, 0.016);
        td += ecs_death(&mut entities); ecs_part(&mut entities, 0.016);
    }
    let ms = start.elapsed().as_millis() as f64;
    bench_result!("total_ms", ms);
    bench_result!("per_frame_ms", ms / ECS_ITERS as f64);
    println!("  Col:{} Filt:{} Death:{}", tc, tf, td);
    bench_end!("ECS_20M");
}

// ============================================================================
// BENCHMARK 2: 2048x2048 Matrix Multiply
// ============================================================================

const MAT_N: usize = 2048;
const MAT_TILE: usize = 64;

#[inline(never)]
fn matmul_tiled(a: &[f64], b: &[f64], c: &mut [f64], n: usize) {
    for ii in (0..n).step_by(MAT_TILE) {
        for jj in (0..n).step_by(MAT_TILE) {
            for kk in (0..n).step_by(MAT_TILE) {
                let ie = std::cmp::min(ii + MAT_TILE, n);
                let je = std::cmp::min(jj + MAT_TILE, n);
                let ke = std::cmp::min(kk + MAT_TILE, n);
                for i in ii..ie {
                    for k in kk..ke {
                        let a_ik = a[i * n + k];
                        for j in jj..je {
                            c[i * n + j] += a_ik * b[k * n + j];
                        }
                    }
                }
            }
        }
    }
}

fn bench_matmul() {
    bench_start!("MATMUL_2048");
    let n = MAT_N; let sz = n * n;
    let a: Vec<f64> = (0..sz).map(|i| (i % 100) as f64 * 0.01).collect();
    let b: Vec<f64> = (0..sz).map(|i| ((i+1) % 100) as f64 * 0.01).collect();
    let mut c: Vec<f64> = vec![0.0; sz];

    let start = Instant::now();
    matmul_tiled(&a, &b, &mut c, n);
    let ms = start.elapsed().as_millis() as f64;

    let cs: f64 = c.iter().sum();
    bench_result!("total_ms", ms);
    println!("  Checksum: {:.4}", cs);
    bench_end!("MATMUL_2048");
}

// ============================================================================
// BENCHMARK 3: 10M Node Graph BFS
// ============================================================================

#[repr(C)]
struct GNode { id: i32, ec: i32, edges: *mut i32, val: f64, vis: i32 }
unsafe impl Send for GNode {}

fn bench_graph_bfs() {
    bench_start!("GRAPH_BFS_10M");
    let nn: usize = 10_000_000;
    let edge_layout = Layout::array::<i32>(nn * 4).unwrap();
    let edges = unsafe { alloc(edge_layout) as *mut i32 };
    let node_layout = Layout::array::<GNode>(nn).unwrap();
    let nodes = unsafe { alloc(node_layout) as *mut GNode };

    unsafe {
        for i in 0..nn {
            (*nodes.add(i)) = GNode { id: i as i32, ec: 4, edges: edges.add(i*4), val: i as f64 * 0.001, vis: 0 };
            for e in 0..4 {
                *edges.add(i*4+e) = if e == 0 && i+1 < nn { (i+1) as i32 } else { ((i as i64 * i as i64) % nn as i64) as i32 };
            }
        }
    }

    let mut total_ms: f64 = 0.0;
    let mut tv: i64 = 0;
    for _ in 0..3 {
        unsafe { for i in 0..nn { (*nodes.add(i)).vis = 0; } }
        let mut queue: Vec<i32> = vec![0i32; nn];
        let (mut qh, mut qt) = (0usize, 1usize);
        queue[0] = 0;
        unsafe { (*nodes.add(0)).vis = 1; }
        let mut vis: i64 = 0;

        let start = Instant::now();
        while qh < qt {
            let nid = queue[qh] as usize; qh += 1; vis += 1;
            let node = unsafe { &*nodes.add(nid) };
            for e in 0..4 {
                let t = unsafe { *node.edges.add(e) } as usize;
                if unsafe { (*nodes.add(t)).vis } == 0 {
                    unsafe { (*nodes.add(t)).vis = 1; }
                    queue[qt] = t as i32; qt += 1;
                }
            }
        }
        total_ms += start.elapsed().as_millis() as f64;
        tv += vis;
    }

    bench_result!("total_ms", total_ms); bench_result!("avg_ms", total_ms / 3.0);
    println!("  Visited(3 rounds): {}", tv);

    unsafe { dealloc(nodes as *mut u8, node_layout); dealloc(edges as *mut u8, edge_layout); }
    bench_end!("GRAPH_BFS_10M");
}

// ============================================================================
// BENCHMARK 4: Allocator Stress — 50M allocations
// ============================================================================

fn bench_allocator() {
    bench_start!("ALLOCATOR_50M");
    let na: usize = 50_000_000;
    println!("  Running 50M allocations...");

    // Arena (Vec-based bump allocator)
    let arena_size: usize = 2 * 1024 * 1024 * 1024;
    let mut arena_buf: Vec<u8> = vec![0u8; arena_size];
    let arena_ptr = arena_buf.as_mut_ptr();
    let mut arena_off: usize = 0;

    let start = Instant::now();
    let mut asum: i64 = 0;
    for i in 0..na {
        let sz = 16 + (i % 4) * 16;
        let aligned = (arena_off + 15) & !15;
        if aligned + sz < arena_size {
            let p = unsafe { arena_ptr.add(aligned) as *mut i64 };
            unsafe { *p = i as i64; asum += *p; }
            arena_off = aligned + sz;
        }
        if i % 1_000_000 == 999_999 { arena_off = 0; }
    }
    let ams = start.elapsed().as_millis() as f64;

    // Heap (Vec)
    let start = Instant::now();
    let mut hsum: i64 = 0;
    for i in 0..na {
        let sz = 16 + (i % 4) * 16;
        let mut v: Vec<u8> = Vec::with_capacity(sz);
        unsafe { v.set_len(sz); }
        let p = v.as_mut_ptr() as *mut i64;
        unsafe { *p = i as i64; hsum += *p; }
        drop(v);
    }
    let hms = start.elapsed().as_millis() as f64;

    bench_result!("arena_ms", ams); bench_result!("heap_ms", hms); bench_result!("speedup", hms / ams);
    println!("  Arena:{} Heap:{}", asum, hsum);
    bench_end!("ALLOCATOR_50M");
}

// ============================================================================
// BENCHMARK 5: Multi-threaded — 8 threads, 2M tasks
// ============================================================================

fn fib(n: i64) -> i64 {
    if n <= 1 { return n; }
    let (mut a, mut b): (i64, i64) = (0, 1);
    for _ in 2..=n { let c = a + b; a = b; b = c; }
    b
}

fn bench_threadpool() {
    bench_start!("THREADPOOL_2M");
    let nt: usize = 2_000_000;
    let nthr: usize = 8;
    println!("  Running {} tasks on {} threads...", nt, nthr);

    let chunk = nt / nthr;
    let result_sum = Arc::new(AtomicI64::new(0));

    let start = Instant::now();
    let mut handles = Vec::new();
    for t in 0..nthr {
        let rs = Arc::clone(&result_sum);
        let s = t * chunk;
        let e = if t == nthr - 1 { nt } else { (t+1) * chunk };
        handles.push(thread::spawn(move || {
            let mut local: i64 = 0;
            for i in s..e { local += fib(20 + ((i as i64) % 20)); }
            rs.fetch_add(local, Ordering::Relaxed);
        }));
    }
    for h in handles { h.join().unwrap(); }
    let ms = start.elapsed().as_millis() as f64;

    bench_result!("total_ms", ms); bench_result!("tasks_per_sec", nt as f64 / (ms / 1000.0));
    println!("  Sum: {}", result_sum.load(Ordering::Relaxed));
    bench_end!("THREADPOOL_2M");
}

// ============================================================================
// BENCHMARK 6: Error Path — 10M calls (Rust Result<T,E>)
// ============================================================================

#[inline(never)]
fn result_fn(input: i32) -> Result<i32, i32> {
    if input % 100 == 0 { Err(42) } else { Ok(input * 3 + 7) }
}

fn bench_error_path() {
    bench_start!("ERROR_10M");
    let nc: usize = 10_000_000;
    println!("  Running {} error-returning calls (1% error)...", nc);

    let mut sum: i64 = 0; let mut errs: i32 = 0;
    let start = Instant::now();
    for i in 0..nc {
        match result_fn(i as i32) {
            Ok(v) => sum += v as i64,
            Err(_) => errs += 1,
        }
    }
    let ms = start.elapsed().as_millis() as f64;

    bench_result!("rust_result_ms", ms);
    println!("  Sum:{} ({} errors)", sum, errs);
    bench_end!("ERROR_10M");
}

// ============================================================================
// BENCHMARK 7: Radix Sort — 50M integers
// ============================================================================

fn radix_sort(data: &mut [i32], temp: &mut [i32]) {
    let n = data.len();
    let mut src_is_data = true;
    for shift in (0..32).step_by(8) {
        let (src, dst) = if src_is_data { (data.as_ptr(), temp.as_mut_ptr()) }
                         else { (temp.as_ptr(), data.as_mut_ptr()) };
        let mut cnt = [0usize; 256];
        unsafe {
            for i in 0..n { cnt[((*src.add(i) >> shift) & 0xFF) as usize] += 1; }
            let mut tot = 0usize;
            for c in cnt.iter_mut() { let old = *c; *c = tot; tot += old; }
            for i in 0..n { let b = ((*src.add(i) >> shift) & 0xFF) as usize; *dst.add(cnt[b]) = *src.add(i); cnt[b] += 1; }
        }
        src_is_data = !src_is_data;
    }
    if !src_is_data { data.copy_from_slice(temp); }
}

fn bench_radix_sort() {
    bench_start!("RADIX_50M");
    let n: usize = 50_000_000;
    println!("  Sorting {} integers...", n);
    let mut data: Vec<i32> = Vec::with_capacity(n);
    let mut rng: u32 = 42;
    for _ in 0..n { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; data.push((rng & 0x7FFFFFFF) as i32); }
    let mut temp: Vec<i32> = vec![0; n];

    let start = Instant::now();
    radix_sort(&mut data, &mut temp);
    let ms = start.elapsed().as_millis() as f64;

    let sorted = data.windows(2).all(|w| w[0] <= w[1]);
    bench_result!("total_ms", ms);
    println!("  Sorted:{} First:{} Last:{}", if sorted {"YES"} else {"NO"}, data[0], data[n-1]);
    bench_end!("RADIX_50M");
}

fn main() {
    println!("======================================================================");
    println!("  Rust Heavy Benchmark — -O -C target-cpu=native -C opt-level=3");
    println!("  7 benches: ECS(20M), MatMul(2K), BFS(10M), Alloc(50M),");
    println!("             ThreadPool(2M), ErrorPath(10M), RadixSort(50M)");
    println!("======================================================================\n");

    bench_ecs(); println!();
    bench_matmul(); println!();
    bench_graph_bfs(); println!();
    bench_allocator(); println!();
    bench_threadpool(); println!();
    bench_error_path(); println!();
    bench_radix_sort();

    println!("\n======================================================================");
    println!("  All benchmarks complete.");
    println!("======================================================================");
}
