// Gust: nbody and raytrace from SPEC.md, each as main.goose runs it and as a
// `parallel for` (spec §6.5) runs it, for compare/run_parallel.py against
// Rust with rayon (rust_parallel/). The parallel forms give the same
// checksums as the sequential ones, bit for bit: each iteration writes its
// own element, and what the elements sum to is summed afterwards in order.
//
//     gust build parallel.goose && parallel [repeats] [name]

import std;
import os;
import math;
import vec;

// ------------------------------------------------------------------ shared

struct Rnd { state: u32 }

fn next(r: Rnd&) -> u32 {
    r.state = r.state * 1664525 + 1013904223;
    return r.state;
}

fn nextf(r: Rnd&) -> f64 { r.next() * (1.0 / 4294967296.0) }
fn range(r: Rnd&, a: f64, b: f64) -> f64 { a + r.nextf() * (b - a) }

type V3 = double3;

fn v3(x: f64, y: f64, z: f64) -> V3 { V3 { x, y, z } }

fn norm(v: V3) -> V3 {
    let l = sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if l == 0.0 { return double3_0; }
    return v3(v.x / l, v.y / l, v.z / l);
}

// ------------------------------------------------------------------ 1. nbody

let N = 256;
let STEPS = 1000;
let DT = 0.001;
let EPS2 = 0.5;

struct Body { p: V3, m: f64 }

fn bodies_of(bodies: Body[>..]&, vel: V3[>..]&) {
    var rnd = Rnd { 12345 };
    for i in N {
        let p = v3(rnd.range(-40.0, 40.0), rnd.range(-40.0, 40.0), rnd.range(-40.0, 40.0));
        let v = v3(rnd.range(-0.5, 0.5), rnd.range(-0.5, 0.5), rnd.range(-0.5, 0.5));
        bodies.push(Body { p, rnd.range(0.2, 1.2) });
        vel.push(v);
    }
}

fn energy(bodies: Body[:], vel: V3[:]) -> f64 {
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

fn advance(bodies: Body[>..]&, vel: V3[>..]&, acc: V3[:]) {
    for &b, i in bodies {
        let v = vel[i] + acc[i] * DT;
        vel[i] = v;
        b.p = b.p + v * DT;
    }
}

fn bench_nbody() -> f64 {
    var bodies: Body[>..] = [];
    var vel: V3[>..] = [];
    bodies_of(bodies, vel);
    var acc: V3[>..] = [];
    acc.push_n(double3_0, N);
    for s in STEPS {
        for bi, i in bodies {
            let pi = bi.p;
            var a = double3_0;
            for bj in bodies {
                let d = bj.p - pi;
                let r2 = dot(d, d) + EPS2;
                a = a + d * (bj.m / (r2 * sqrt(r2)));
            }
            acc[i] = a;
        }
        advance(bodies, vel, acc);
    }
    return energy(bodies, vel);
}

// Each body's acceleration on a worker: it reads every body, which no
// iteration writes, and writes its own element of acc.
fn bench_nbody_par() -> f64 {
    var bodies: Body[>..] = [];
    var vel: V3[>..] = [];
    bodies_of(bodies, vel);
    var acc: V3[>..] = [];
    acc.push_n(double3_0, N);
    for s in STEPS {
        parallel for i in N {
            let pi = bodies[i].p;
            var a = double3_0;
            for bj in bodies {
                let d = bj.p - pi;
                let r2 = dot(d, d) + EPS2;
                a = a + d * (bj.m / (r2 * sqrt(r2)));
            }
            acc[i] = a;
        }
        advance(bodies, vel, acc);
    }
    return energy(bodies, vel);
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

fn spheres_of(spheres: Sphere[>..]&) {
    var rnd = Rnd { 98765 };
    for i in NSPHERES {
        let c = v3(rnd.range(-6.0, 6.0), rnd.range(0.4, 4.5), rnd.range(-6.0, 6.0));
        let r = rnd.range(0.35, 0.9);
        let col = v3(rnd.range(0.2, 1.0), rnd.range(0.2, 1.0), rnd.range(0.2, 1.0));
        spheres.push(Sphere { c, r, col, if i % 3 == 0 { 0.55 } else { 0.0 } });
    }
}

fn bench_raytrace() -> f64 {
    var spheres: Sphere[>..] = [];
    spheres_of(spheres);
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

// Each pixel on a worker, into its own element; the sum of the pixels in
// the order the sequential loop adds them.
fn bench_raytrace_par() -> f64 {
    var spheres: Sphere[>..] = [];
    spheres_of(spheres);
    let light = norm(v3(-0.6, 1.0, 0.45));
    let eye = v3(0.0, 2.6, 9.0);
    let fwd = norm(v3(0.0, 1.2, 0.0) - eye);
    let right = norm(cross(fwd, v3(0.0, 1.0, 0.0)));
    let cam_up = cross(right, fwd);
    let aspect = RT_W / (RT_H as f64);
    var pixels: f64[>..] = [];
    pixels.push_n(0.0, RT_W * RT_H);
    parallel for k in RT_W * RT_H {
        let py = k / RT_W;
        let px = k % RT_W;
        let v = 1.0 - (py + 0.5) / RT_H * 2.0;
        let u = (px + 0.5) / RT_W * 2.0 - 1.0;
        let rd = norm(fwd + right * (u * aspect * HALF) + cam_up * (v * HALF));
        let col = trace(spheres, light, eye, rd, MAX_DEPTH);
        pixels[k] = col.x + col.y + col.z;
    }
    var sum = 0.0;
    for x in pixels { sum += x; }
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
    run("nbody_par", repeats, only, bench_nbody_par);
    run("raytrace", repeats, only, bench_raytrace);
    run("raytrace_par", repeats, only, bench_raytrace_par);
}
