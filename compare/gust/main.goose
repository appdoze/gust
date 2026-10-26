// Goose implementation of the cross-language benchmarks: 1-5 follow
// lobster/tests/compare/SPEC.md, 6-8 follow bench/EXTRA_SPEC.md.
//
//     goose build main.goose && main [repeats] [name]

import std;
import os;
import math;
import vec;
import dictionary;

// ------------------------------------------------------------------ shared

// The 32-bit LCG from SPEC.md. Unsigned arithmetic wraps, which is exactly
// the modulo 2^32 it asks for.
struct Rnd { state: u32 }

fn next(r: Rnd&) -> u32 {
    r.state = r.state * 1664525 + 1013904223;
    return r.state;
}

fn nextf(r: Rnd&) -> f64 { r.next() * (1.0 / 4294967296.0) }
fn range(r: Rnd&, a: f64, b: f64) -> f64 { a + r.nextf() * (b - a) }
fn below(r: Rnd&, n: i64) -> i64 { r.next() % n }

type V3 = double3;

fn v3(x: f64, y: f64, z: f64) -> V3 { V3 { x, y, z } }

// Spelled out rather than taken from `vec`, to divide each component by the
// length exactly as the other implementations do.
fn norm(v: V3) -> V3 {
    let l = sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if l == 0.0 { return double3_0; }
    return v3(v.x / l, v.y / l, v.z / l);
}

// ------------------------------------------------------------------ 1. nbody

struct Body { p: V3, m: f64 }

fn bench_nbody() -> f64 {
    let N = 256;
    let STEPS = 1000;
    let DT = 0.001;
    let EPS2 = 0.5;
    var rnd = Rnd { 12345 };
    var bodies: Body[>..] = [];
    var vel: V3[>..] = [];
    for i in N {
        let p = v3(rnd.range(-40.0, 40.0), rnd.range(-40.0, 40.0), rnd.range(-40.0, 40.0));
        let v = v3(rnd.range(-0.5, 0.5), rnd.range(-0.5, 0.5), rnd.range(-0.5, 0.5));
        bodies.push(Body { p, rnd.range(0.2, 1.2) });
        vel.push(v);
    }
    var acc: V3[>..] = [];
    acc.push_n(double3_0, N);
    for s in STEPS {
        for bi, i in bodies {
            let pi = bi.p;
            var a = double3_0;
            for bj in bodies {
                let d = bj.p - pi;
                let r2 = dot(d, d) + EPS2;
                // The j == i term has d == 0, so it contributes exactly zero.
                a = a + d * (bj.m / (r2 * sqrt(r2)));
            }
            acc[i] = a;
        }
        for &b, i in bodies {
            let v = vel[i] + acc[i] * DT;
            vel[i] = v;
            b.p = b.p + v * DT;
        }
    }
    var e = 0.0;
    for b, i in bodies { e += 0.5 * b.m * dot(vel[i], vel[i]); }
    for bi, i in bodies {
        for bj, j in bodies {
            if j > i {
                let d = bj.p - bi.p;
                e -= bi.m * bj.m / sqrt(dot(d, d) + EPS2);
            }
        }
    }
    return e;
}

// ------------------------------------------------------------------ 2. raytrace

let RT_W = 640;
let RT_H = 480;
let NSPHERES = 48;
let MAX_DEPTH = 3;
let EPS = 0.0001;
let HALF = 0.5205670505517462;          // tan(27.5 degrees)
let AMBIENT = 0.12;

struct Sphere { c: V3, r: f64, col: V3, refl: f64 }

// What a ray found, with a negative t meaning it hit nothing.
struct Hit { t: f64, n: V3, albedo: V3, refl: f64 }

fn hit_sphere(s: Sphere&, ro: V3, rd: V3) -> f64 {
    let oc = s.c - ro;
    let b = dot(oc, rd);
    let c = dot(oc, oc) - s.r * s.r;
    let disc = b * b - c;
    guard disc > 0.0 else { return -1.0; }
    let sd = sqrt(disc);
    let t = b - sd;
    if t > EPS { return t; }
    let t2 = b + sd;
    return if t2 > EPS { t2 } else { -1.0 };
}

fn any_hit(spheres: Sphere[:], ro: V3, rd: V3) -> bool {
    for &s in spheres {
        if hit_sphere(s, ro, rd) > 0.0 { return true; }
    }
    return false;
}

fn nearest(spheres: Sphere[:], ro: V3, rd: V3) -> Hit {
    var best = Hit { t: -1.0, n: double3_0, albedo: double3_0, refl: 0.0 };
    for &s in spheres {
        let t = hit_sphere(s, ro, rd);
        if t > 0.0 && (best.t < 0.0 || t < best.t) {
            best = Hit { t: t, n: norm(ro + rd * t - s.c), albedo: s.col, refl: s.refl };
        }
    }
    // The ground plane, which only faces up and so is only hit by downward rays.
    if rd.y < -1.0e-9 {
        let t = -ro.y / rd.y;
        if t > EPS && (best.t < 0.0 || t < best.t) {
            let p = ro + rd * t;
            let shade = if (floor(p.x) + floor(p.z)) % 2.0 == 0.0 { 0.9 } else { 0.35 };
            best = Hit { t: t, n: v3(0.0, 1.0, 0.0), albedo: v3(shade, shade, shade), refl: 0.25 };
        }
    }
    return best;
}

recursive fn trace(spheres: Sphere[:], light: V3, ro: V3, rd: V3, depth: i64) -> V3 {
    let h = nearest(spheres, ro, rd);
    guard h.t > 0.0 else {
        return v3(0.35, 0.52, 0.85) * (0.4 + 0.6 * max(0.0, rd.y));
    }
    let p = ro + rd * h.t;
    let off = p + h.n * EPS;
    let lit = if any_hit(spheres, off, light) { 0.0 } else { 1.0 };
    let shaded = h.albedo * (AMBIENT + max(0.0, dot(h.n, light)) * lit);
    guard h.refl > 0.0 && depth > 0 else { return shaded; }
    let refd = rd - h.n * (2.0 * dot(h.n, rd));
    return shaded * (1.0 - h.refl) + trace(spheres, light, off, refd, depth - 1) * h.refl;
}

fn bench_raytrace() -> f64 {
    var rnd = Rnd { 98765 };
    var spheres: Sphere[>..] = [];
    for i in NSPHERES {
        let c = v3(rnd.range(-6.0, 6.0), rnd.range(0.4, 4.5), rnd.range(-6.0, 6.0));
        let r = rnd.range(0.35, 0.9);
        let col = v3(rnd.range(0.2, 1.0), rnd.range(0.2, 1.0), rnd.range(0.2, 1.0));
        spheres.push(Sphere { c, r, col, if i % 3 == 0 { 0.55 } else { 0.0 } });
    }
    let light = norm(v3(-0.6, 1.0, 0.45));
    let eye = v3(0.0, 2.6, 9.0);
    let fwd = norm(v3(0.0, 1.2, 0.0) - eye);
    let right = norm(cross(fwd, v3(0.0, 1.0, 0.0)));
    let cam_up = cross(right, fwd);
    let aspect = RT_W / (RT_H as f64);
    var sum = 0.0;
    for py in RT_H {
        let v = 1.0 - (py + 0.5) / RT_H * 2.0;
        for px in RT_W {
            let u = (px + 0.5) / RT_W * 2.0 - 1.0;
            let rd = norm(fwd + right * (u * aspect * HALF) + cam_up * (v * HALF));
            let col = trace(spheres, light, eye, rd, MAX_DEPTH);
            sum += col.x + col.y + col.z;
        }
    }
    return sum;
}

// ------------------------------------------------------------------ 3. interp

let IP_DEPTH = 14;
let IP_ROUNDS = 1500;
let NVARS = 8;

// Variable-mode nodes in one pool, linked by 4-byte self-relative references:
// the way Goose spells a tree.
enum Expr {
    Const { v: f64 },
    Var { i: i32 },
    Neg { a: Expr..&<u32> },
    Add { a: Expr..&<u32>, b: Expr..&<u32> },
    Sub { a: Expr..&<u32>, b: Expr..&<u32> },
    Mul { a: Expr..&<u32>, b: Expr..&<u32> },
}

// The node's own draw comes before its children are built, so the draws are
// in pre-order, while the node itself is pushed after its children.
recursive fn build(pool: Expr..[>..]&, rnd: Rnd&, d: i64) -> Expr..& {
    if d == 0 {
        let k = rnd.below(3);
        if k == 0 { return pool.push(Expr.Const { v: rnd.range(-1.0, 1.0) }); }
        if k == 1 { return pool.push(Expr.Var { i: rnd.below(NVARS) as i32 }); }
        let a: Expr..& = build(pool, rnd, 0);
        return pool.push(Expr.Neg { a: a });
    }
    // Multiplication only near the leaves, so the value of the tree stays finite.
    let k = rnd.below(if d <= 3 { 3 } else { 2 });
    let a: Expr..& = build(pool, rnd, d - 1);
    let b: Expr..& = build(pool, rnd, d - 1);
    if k == 0 { return pool.push(Expr.Add { a: a, b: b }); }
    if k == 1 { return pool.push(Expr.Sub { a: a, b: b }); }
    return pool.push(Expr.Mul { a: a, b: b });
}

recursive fn eval(e: Expr..&, env: f64[:]) -> f64 {
    match e {
        Const &c => c.v,
        Var &x => env[x.i],
        Neg &n => -eval(n.a, env),
        Add &x => eval(x.a, env) + eval(x.b, env),
        Sub &x => eval(x.a, env) - eval(x.b, env),
        Mul &x => eval(x.a, env) * eval(x.b, env),
    }
}

fn bench_interp() -> f64 {
    var rnd = Rnd { 4242 };
    var pool: Expr..[>..] = [];
    let tree: Expr..& = build(pool, rnd, IP_DEPTH);
    var env: f64[8] = [0.0; 8];
    var total = 0.0;
    for r in IP_ROUNDS {
        for k in NVARS { env[k] = ((r * 7 + k * 13) % 101) / 50.0 - 1.0; }
        total += eval(tree, env);
    }
    return total;
}

// ------------------------------------------------------------------ 4. sort

let SORT_N = 400000;
let SORT_ROUNDS = 6;

struct Rec { key: f64, id: i64 }

// The comparison is a function value, a compile-time entity, so it is
// specialized into the partition loop.
recursive fn quicksort<F>(a: Rec[:], lo: i64, hi: i64) {
    if hi - lo <= 1 { return; }
    let pivot = a[lo];
    var sp = lo + 1;
    var ep = hi;
    while sp < ep {
        let c = a[sp];
        if F(c, pivot) {
            a[sp - 1] = a[sp];
            sp += 1;
        } else {
            ep -= 1;
            a[sp] = a[ep];
            a[ep] = c;
        }
    }
    sp -= 1;
    a[sp] = pivot;
    quicksort(a, lo, sp, F);
    quicksort(a, ep, hi, F);
}

fn less(a: Rec, b: Rec) -> bool { a.key < b.key || (a.key == b.key && a.id < b.id) }

fn bench_sort() -> f64 {
    var rnd = Rnd { 777 };
    var src: Rec[>..] = [];
    for i in SORT_N { src.push(Rec { key: rnd.range(-1000.0, 1000.0), id: i }); }
    var sum = 0.0;
    for round in SORT_ROUNDS {
        var xs = copy(src);
        quicksort(xs, 0, xs.len) { a, b => a.key < b.key || (a.key == b.key && a.id < b.id) };
        // Only the last round's result is checked, as in the other implementations.
        if round == SORT_ROUNDS - 1 {
            for i in 1..xs.len {
                if less(xs[i], xs[i - 1]) {
                    print("# sort: not sorted at ", i);
                    return 0.0;
                }
            }
            for r, i in xs { sum += (i % 97 + 1) * r.id; }
        }
    }
    return sum;
}

// ------------------------------------------------------------------ 5. astar

let AS_W = 512;
let AS_H = 512;
let AS_ROUNDS = 20;
let WALL_P = 0.20;
let INF = 0x3FFFFFFF;

struct HeapEnt { f: i64, n: i64 }

fn before(a: HeapEnt, b: HeapEnt) -> bool { a.f < b.f || (a.f == b.f && a.n < b.n) }

fn hpush(heap: HeapEnt[>..<]&, e: HeapEnt) {
    heap.push(e);
    var i = heap.len - 1;
    while i > 0 {
        let p = (i - 1) >> 1;
        let pe = heap[p];
        if !before(e, pe) { break; }
        heap[i] = pe;
        i = p;
    }
    heap[i] = e;
}

fn hpop(heap: HeapEnt[>..<]&) -> HeapEnt {
    let top = heap[0];
    let last = heap.pop();
    let n = heap.len;
    if n > 0 {
        var i = 0;
        loop {
            let l = (i << 1) + 1;
            if l >= n { break; }
            var c = l;
            let r = l + 1;
            if r < n && before(heap[r], heap[l]) { c = r; }
            let ce = heap[c];
            if !before(ce, last) { break; }
            heap[i] = ce;
            i = c;
        }
        heap[i] = last;
    }
    return top;
}

// The cost of the best path and the number of nodes expanded, with a cost
// of -1 where the goal could not be reached.
fn solve(seed: u32) -> i64, i64 {
    var rnd = Rnd { seed };
    let cells = AS_W * AS_H;
    var grid: i64[>..] = [];
    for i in cells { grid.push(if rnd.nextf() < WALL_P { 1 } else { 0 }); }
    let goal = cells - 1;
    grid[0] = 0;
    grid[1] = 0;
    grid[AS_W] = 0;
    grid[goal] = 0;
    grid[goal - 1] = 0;
    grid[goal - AS_W] = 0;
    var gscore: i64[>..] = [];
    gscore.push_n(INF, cells);
    var closed: i64[>..] = [];
    closed.push_n(0, cells);
    var heap: HeapEnt[>..<] = [];
    let DX: i64[4] = [1, 0, -1, 0];
    let DY: i64[4] = [0, 1, 0, -1];
    let gx = AS_W - 1;
    let gy = AS_H - 1;
    gscore[0] = 0;
    hpush(heap, HeapEnt { f: gx + gy, n: 0 });
    var expanded = 0;
    var cost = -1;
    while heap.len > 0 {
        let e = hpop(heap);
        let n = e.n;
        if closed[n] == 0 {
            closed[n] = 1;
            expanded += 1;
            if n == goal {
                cost = gscore[n];
                break;
            }
            let y = n / AS_W;
            let x = n - y * AS_W;
            let g = gscore[n] + 1;
            for d in 4 {
                let nx = x + DX[d];
                let ny = y + DY[d];
                if nx >= 0 && ny >= 0 && nx < AS_W && ny < AS_H {
                    let nb = n + DY[d] * AS_W + DX[d];
                    if grid[nb] == 0 && closed[nb] == 0 && g < gscore[nb] {
                        gscore[nb] = g;
                        // Neither coordinate can exceed the goal's, so this needs no abs().
                        hpush(heap, HeapEnt { f: g + (gx - nx) + (gy - ny), n: nb });
                    }
                }
            }
        }
    }
    return cost, expanded;
}

fn bench_astar() -> f64 {
    var sum = 0;
    for r in AS_ROUNDS {
        let cost, expanded = solve((1000 + r) as u32);
        sum += cost * 1000003 + expanded;
    }
    return sum;
}

// ------------------------------------------------------------------ 6. bintrees

let MIN_DEPTH = 4;
let BT_MAX_DEPTH = 16;

// A tree is a pool of 8-byte nodes local to the function that builds it,
// linked by self-relative references; it is freed when that function returns.
struct Node { l: Node&<u32>?, r: Node&<u32>? }

recursive fn make(pool: Node[>..]&, depth: i64) -> Node& {
    if depth == 0 { return pool.push(Node {}); }
    let l: Node& = make(pool, depth - 1);
    let r: Node& = make(pool, depth - 1);
    return pool.push(Node { l: l, r: r });
}

recursive fn check(n: Node&) -> i64 {
    let l = n.l;
    guard l else { return 1; }
    let r = n.r;
    guard r else { return 1; }
    return 1 + check(l) + check(r);
}

fn tree_check(depth: i64) -> i64 {
    var pool: Node[>..] = [];
    return check(make(pool, depth));
}

fn bench_bintrees() -> f64 {
    var total = tree_check(BT_MAX_DEPTH + 1);
    var long: Node[>..] = [];
    let long_root: Node& = make(long, BT_MAX_DEPTH);
    var d = MIN_DEPTH;
    while d <= BT_MAX_DEPTH {
        let iterations = 1 << (BT_MAX_DEPTH - d + MIN_DEPTH);
        for i in iterations { total += tree_check(d); }
        d += 2;
    }
    total += check(long_root);
    return total;
}

// ------------------------------------------------------------------ 7. hashmap

let HM_ROUNDS = 5;
let N_OPS = 1000000;
let KEYS = 131072;

fn bench_hashmap() -> f64 {
    var sum = 0;
    for round in HM_ROUNDS {
        var rnd = Rnd { (31337 + round) as u32 };
        var counts = dictionary<i64, i64> {};
        counts.reserve(KEYS);
        for i in N_OPS { counts.update(rnd.below(KEYS), 0) { it += 1; }; }
        var found = 0;
        var misses = 0;
        for i in N_OPS {
            let v = counts.get(rnd.below(2 * KEYS));
            if v { found += v; } else { misses += 1; }
        }
        sum += counts.count * 1000003 + found * 3 + misses;
    }
    return sum;
}

// ------------------------------------------------------------------ 8. strings

let ST_ROUNDS = 10;
let NWORDS = 200000;
let VOCAB = 5000;

fn bench_strings() -> f64 {
    var sum = 0;
    for round in ST_ROUNDS {
        var rnd = Rnd { (2024 + round) as u32 };
        var text: u8[>..] = [];
        for i in NWORDS {
            if i > 0 { text.push(' '); }
            format(text, "w", rnd.below(VOCAB));
        }
        // The keys are slices into `text`: no word is copied.
        var counts = dictionary<u8[:], i64> {};
        counts.reserve(VOCAB);
        each_split(text, ' ') { counts.update(it, 0) { it += 1; }; };
        var distinct = 0;
        var sq = 0;
        var key: u8[..16] = [];
        for v in VOCAB {
            key.clear();
            format(key, "w", v);
            let c = counts.get(key);
            if c && c > 0 {
                distinct += 1;
                sq += c * c;
            }
        }
        sum += text.len + distinct * 1000 + sq;
    }
    return sum;
}

// ------------------------------------------------------------------ driver

fn run<F>(name: u8[:], repeats: i64, only: u8[:]) {
    if only.len > 0 && only != name { return; }
    var best = 1.0e30;
    var checksum = 0.0;
    for i in repeats {
        let t0 = clock();
        checksum = F();
        best = min(best, clock() - t0);
    }
    print("bench\t", name, "\t", best, "\t", checksum);
}

fn main() {
    let a = args();
    var repeats = 5;
    var only = "";
    if a.len > 1 {
        let n, ok = parse_int(a[1]);
        if ok && n > 0 { repeats = n; }
    }
    if a.len > 2 { only = a[2]; }
    run("nbody", repeats, only, bench_nbody);
    run("raytrace", repeats, only, bench_raytrace);
    run("interp", repeats, only, bench_interp);
    run("sort", repeats, only, bench_sort);
    run("astar", repeats, only, bench_astar);
    run("bintrees", repeats, only, bench_bintrees);
    run("hashmap", repeats, only, bench_hashmap);
    run("strings", repeats, only, bench_strings);
}
