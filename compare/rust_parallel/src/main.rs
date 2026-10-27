// Rust with rayon: nbody and raytrace from ../../SPEC.md, each as
// ../../rust/main.rs runs it and with rayon's parallel iterators, for
// ../../run_parallel.py against Gust's `parallel for` (../../gust/parallel.goose).
// The parallel forms give the same checksums as the sequential ones, bit for
// bit: each iteration writes its own element, summed afterwards in order.
//
//     cargo build --release && target/release/rust_parallel [repeats] [name]

use rayon::prelude::*;
use std::time::Instant;

// ------------------------------------------------------------------ shared

struct Rnd {
    state: u32,
}

impl Rnd {
    fn new(seed: u32) -> Rnd {
        Rnd { state: seed }
    }
    fn next(&mut self) -> u32 {
        self.state = self.state.wrapping_mul(1664525).wrapping_add(1013904223);
        self.state
    }
    fn nextf(&mut self) -> f64 {
        self.next() as f64 * (1.0 / 4294967296.0)
    }
    fn range(&mut self, a: f64, b: f64) -> f64 {
        a + self.nextf() * (b - a)
    }
}

#[derive(Clone, Copy)]
struct V3 {
    x: f64,
    y: f64,
    z: f64,
}

impl V3 {
    fn new(x: f64, y: f64, z: f64) -> V3 {
        V3 { x, y, z }
    }
    fn zero() -> V3 {
        V3 { x: 0.0, y: 0.0, z: 0.0 }
    }
}

impl std::ops::Add for V3 {
    type Output = V3;
    fn add(self, o: V3) -> V3 {
        V3::new(self.x + o.x, self.y + o.y, self.z + o.z)
    }
}
impl std::ops::Sub for V3 {
    type Output = V3;
    fn sub(self, o: V3) -> V3 {
        V3::new(self.x - o.x, self.y - o.y, self.z - o.z)
    }
}
impl std::ops::Mul<f64> for V3 {
    type Output = V3;
    fn mul(self, s: f64) -> V3 {
        V3::new(self.x * s, self.y * s, self.z * s)
    }
}

fn dot(a: V3, b: V3) -> f64 {
    a.x * b.x + a.y * b.y + a.z * b.z
}
fn cross(a: V3, b: V3) -> V3 {
    V3::new(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x)
}
fn normalize(v: V3) -> V3 {
    let l = (v.x * v.x + v.y * v.y + v.z * v.z).sqrt();
    if l == 0.0 {
        V3::zero()
    } else {
        V3::new(v.x / l, v.y / l, v.z / l)
    }
}

// ------------------------------------------------------------------ 1. nbody

const N: usize = 256;
const STEPS: usize = 1000;
const DT: f64 = 0.001;
const EPS2: f64 = 0.5;

#[derive(Clone, Copy)]
struct Body {
    p: V3,
    m: f64,
}

fn bodies_of() -> (Vec<Body>, Vec<V3>) {
    let mut rnd = Rnd::new(12345);
    let mut bodies = Vec::with_capacity(N);
    let mut vel = Vec::with_capacity(N);
    for _ in 0..N {
        let p = V3::new(rnd.range(-40.0, 40.0), rnd.range(-40.0, 40.0), rnd.range(-40.0, 40.0));
        let v = V3::new(rnd.range(-0.5, 0.5), rnd.range(-0.5, 0.5), rnd.range(-0.5, 0.5));
        bodies.push(Body { p, m: rnd.range(0.2, 1.2) });
        vel.push(v);
    }
    (bodies, vel)
}

fn accel(bodies: &[Body], pi: V3) -> V3 {
    let mut a = V3::zero();
    for bj in bodies {
        let d = bj.p - pi;
        let r2 = dot(d, d) + EPS2;
        a = a + d * (bj.m / (r2 * r2.sqrt()));
    }
    a
}

fn advance(bodies: &mut [Body], vel: &mut [V3], acc: &[V3]) {
    for i in 0..N {
        let v = vel[i] + acc[i] * DT;
        vel[i] = v;
        bodies[i].p = bodies[i].p + v * DT;
    }
}

fn energy(bodies: &[Body], vel: &[V3]) -> f64 {
    let mut e = 0.0;
    for i in 0..N {
        e += 0.5 * bodies[i].m * dot(vel[i], vel[i]);
    }
    for i in 0..N {
        for j in 0..N {
            if j > i {
                let d = bodies[j].p - bodies[i].p;
                e -= bodies[i].m * bodies[j].m / (dot(d, d) + EPS2).sqrt();
            }
        }
    }
    e
}

fn bench_nbody() -> f64 {
    let (mut bodies, mut vel) = bodies_of();
    let mut acc = vec![V3::zero(); N];
    for _ in 0..STEPS {
        for i in 0..N {
            acc[i] = accel(&bodies, bodies[i].p);
        }
        advance(&mut bodies, &mut vel, &acc);
    }
    energy(&bodies, &vel)
}

fn bench_nbody_par() -> f64 {
    let (mut bodies, mut vel) = bodies_of();
    let mut acc = vec![V3::zero(); N];
    for _ in 0..STEPS {
        acc.par_iter_mut().enumerate().for_each(|(i, a)| *a = accel(&bodies, bodies[i].p));
        advance(&mut bodies, &mut vel, &acc);
    }
    energy(&bodies, &vel)
}

// ------------------------------------------------------------------ 2. raytrace

const RT_W: usize = 640;
const RT_H: usize = 480;
const NSPHERES: usize = 48;
const MAX_DEPTH: i32 = 3;
const EPS: f64 = 0.0001;
/// tan(27.5 degrees).
const HALF: f64 = 0.5205670505517462;
const AMBIENT: f64 = 0.12;

#[derive(Clone, Copy)]
struct Sphere {
    c: V3,
    r: f64,
    col: V3,
    refl: f64,
}

#[derive(Clone, Copy)]
struct Hit {
    t: f64,
    n: V3,
    albedo: V3,
    refl: f64,
}

fn hit_sphere(s: &Sphere, ro: V3, rd: V3) -> f64 {
    let oc = s.c - ro;
    let b = dot(oc, rd);
    let c = dot(oc, oc) - s.r * s.r;
    let disc = b * b - c;
    if !(disc > 0.0) {
        return -1.0;
    }
    let sd = disc.sqrt();
    let t = b - sd;
    if t > EPS {
        return t;
    }
    let t2 = b + sd;
    if t2 > EPS {
        t2
    } else {
        -1.0
    }
}

fn any_hit(spheres: &[Sphere], ro: V3, rd: V3) -> bool {
    spheres.iter().any(|s| hit_sphere(s, ro, rd) > 0.0)
}

fn nearest(spheres: &[Sphere], ro: V3, rd: V3) -> Hit {
    let mut best = Hit { t: -1.0, n: V3::zero(), albedo: V3::zero(), refl: 0.0 };
    for s in spheres {
        let t = hit_sphere(s, ro, rd);
        if t > 0.0 && (best.t < 0.0 || t < best.t) {
            best = Hit { t, n: normalize(ro + rd * t - s.c), albedo: s.col, refl: s.refl };
        }
    }
    if rd.y < -1.0e-9 {
        let t = -ro.y / rd.y;
        if t > EPS && (best.t < 0.0 || t < best.t) {
            let p = ro + rd * t;
            let fx = p.x.floor() as i64;
            let fz = p.z.floor() as i64;
            let shade = if (fx + fz) % 2 == 0 { 0.9 } else { 0.35 };
            best = Hit { t, n: V3::new(0.0, 1.0, 0.0), albedo: V3::new(shade, shade, shade), refl: 0.25 };
        }
    }
    best
}

fn trace(spheres: &[Sphere], light: V3, ro: V3, rd: V3, depth: i32) -> V3 {
    let h = nearest(spheres, ro, rd);
    if !(h.t > 0.0) {
        let s = if rd.y > 0.0 { rd.y } else { 0.0 };
        return V3::new(0.35, 0.52, 0.85) * (0.4 + 0.6 * s);
    }
    let p = ro + rd * h.t;
    let off = p + h.n * EPS;
    let lit = if any_hit(spheres, off, light) { 0.0 } else { 1.0 };
    let mut nl = dot(h.n, light);
    if nl < 0.0 {
        nl = 0.0;
    }
    let shaded = h.albedo * (AMBIENT + nl * lit);
    if !(h.refl > 0.0 && depth > 0) {
        return shaded;
    }
    let refd = rd - h.n * (2.0 * dot(h.n, rd));
    shaded * (1.0 - h.refl) + trace(spheres, light, off, refd, depth - 1) * h.refl
}

struct Scene {
    spheres: Vec<Sphere>,
    light: V3,
    eye: V3,
    fwd: V3,
    right: V3,
    cam_up: V3,
    aspect: f64,
}

fn scene() -> Scene {
    let mut rnd = Rnd::new(98765);
    let mut spheres = Vec::with_capacity(NSPHERES);
    for i in 0..NSPHERES {
        let c = V3::new(rnd.range(-6.0, 6.0), rnd.range(0.4, 4.5), rnd.range(-6.0, 6.0));
        let r = rnd.range(0.35, 0.9);
        let col = V3::new(rnd.range(0.2, 1.0), rnd.range(0.2, 1.0), rnd.range(0.2, 1.0));
        spheres.push(Sphere { c, r, col, refl: if i % 3 == 0 { 0.55 } else { 0.0 } });
    }
    let eye = V3::new(0.0, 2.6, 9.0);
    let fwd = normalize(V3::new(0.0, 1.2, 0.0) - eye);
    let right = normalize(cross(fwd, V3::new(0.0, 1.0, 0.0)));
    Scene {
        spheres,
        light: normalize(V3::new(-0.6, 1.0, 0.45)),
        eye,
        fwd,
        right,
        cam_up: cross(right, fwd),
        aspect: RT_W as f64 / RT_H as f64,
    }
}

fn pixel(sc: &Scene, px: usize, py: usize) -> f64 {
    let v = 1.0 - (py as f64 + 0.5) / RT_H as f64 * 2.0;
    let u = (px as f64 + 0.5) / RT_W as f64 * 2.0 - 1.0;
    let rd = normalize(sc.fwd + sc.right * (u * sc.aspect * HALF) + sc.cam_up * (v * HALF));
    let col = trace(&sc.spheres, sc.light, sc.eye, rd, MAX_DEPTH);
    col.x + col.y + col.z
}

fn bench_raytrace() -> f64 {
    let sc = scene();
    let mut sum = 0.0;
    for py in 0..RT_H {
        for px in 0..RT_W {
            sum += pixel(&sc, px, py);
        }
    }
    sum
}

fn bench_raytrace_par() -> f64 {
    let sc = scene();
    let pixels: Vec<f64> = (0..RT_W * RT_H).into_par_iter().map(|k| pixel(&sc, k % RT_W, k / RT_W)).collect();
    let mut sum = 0.0;
    for x in pixels {
        sum += x;
    }
    sum
}

// ------------------------------------------------------------------ driver

fn run(name: &str, repeats: usize, only: &str, f: fn() -> f64) {
    if !only.is_empty() && only != name {
        return;
    }
    let mut best = f64::MAX;
    let mut checksum = 0.0;
    for _ in 0..repeats {
        let t0 = Instant::now();
        checksum = std::hint::black_box(f());
        best = best.min(t0.elapsed().as_secs_f64());
    }
    println!("bench\t{}\t{}\t{}", name, best, checksum);
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let repeats = args.get(1).and_then(|s| s.parse().ok()).filter(|&n| n > 0).unwrap_or(5);
    let only = args.get(2).map(|s| s.as_str()).unwrap_or("");
    run("nbody", repeats, only, bench_nbody);
    run("nbody_par", repeats, only, bench_nbody_par);
    run("raytrace", repeats, only, bench_raytrace);
    run("raytrace_par", repeats, only, bench_raytrace_par);
}
