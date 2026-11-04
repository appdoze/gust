// Sorting in several shapes, against sortshapes.goose: the numbers are the same in both.
use std::env;
use std::time::Instant;

struct Rnd {
    state: u32,
}
impl Rnd {
    fn next(&mut self) -> u32 {
        self.state = self.state.wrapping_mul(1664525).wrapping_add(1013904223);
        self.state
    }
}

#[derive(Clone, Copy)]
struct Pair {
    k: i64,
    v: i64,
}

const N: i64 = 3000000;
const M: i64 = 1000000;

fn random() -> f64 {
    let mut r = Rnd { state: 7 };
    let mut xs: Vec<i64> = Vec::new();
    for _ in 0..N { xs.push(r.next() as i64); }
    xs.sort_unstable();
    (xs[0] + xs[xs.len() / 2]) as f64
}

fn few() -> f64 {
    let mut r = Rnd { state: 7 };
    let mut xs: Vec<i64> = Vec::new();
    for _ in 0..N { xs.push((r.next() % 1000) as i64); }
    xs.sort_unstable();
    (xs[0] + xs[xs.len() / 2]) as f64
}

fn nearly() -> f64 {
    let mut r = Rnd { state: 7 };
    let mut xs: Vec<i64> = Vec::new();
    for i in 0..N { xs.push(if i % 100 == 0 { r.next() as i64 } else { i }); }
    xs.sort_unstable();
    (xs[0] + xs[xs.len() / 2]) as f64
}

fn saw() -> f64 {
    let mut xs: Vec<i64> = Vec::new();
    for i in 0..N { xs.push(i % 1024); }
    xs.sort_unstable();
    (xs[0] + xs[xs.len() / 2]) as f64
}

fn pipe() -> f64 {
    let mut xs: Vec<i64> = Vec::new();
    for i in 0..N { xs.push(if i < N / 2 { i } else { N - i }); }
    xs.sort_unstable();
    (xs[0] + xs[xs.len() / 2]) as f64
}

fn small32() -> f64 {
    let mut r = Rnd { state: 7 };
    let mut xs: Vec<i32> = Vec::new();
    for _ in 0..N { xs.push((r.next() % 2000000000) as i32); }
    xs.sort_unstable();
    xs[0] as f64 + xs[xs.len() / 2] as f64
}

fn reals() -> f64 {
    let mut r = Rnd { state: 7 };
    let mut xs: Vec<f64> = Vec::new();
    for _ in 0..N { xs.push(r.next() as f64); }
    xs.sort_unstable_by(|a, b| a.partial_cmp(b).unwrap());
    xs[0] + xs[xs.len() / 2]
}

fn tiny() -> f64 {
    let mut r = Rnd { state: 7 };
    let mut xs = vec![0i64; 64];
    let mut sum = 0i64;
    for round in 0..300000usize {
        let n = 2 + round % 60;
        for x in xs[..n].iter_mut() { *x = r.next() as i64; }
        xs[..n].sort_unstable();
        sum += xs[n / 2] & 1;
    }
    sum as f64
}

fn keyed() -> f64 {
    let mut r = Rnd { state: 7 };
    let mut xs: Vec<Pair> = Vec::new();
    for i in 0..M { xs.push(Pair { k: r.next() as i64, v: i }); }
    xs.sort_unstable_by(|a, b| a.k.cmp(&b.k));
    let extra = if xs[0].v < 0 { 1 } else { 0 };
    (xs[0].k + xs[xs.len() / 2].k + extra) as f64
}

fn main() {
    let a: Vec<String> = env::args().collect();
    let only = if a.len() > 2 { a[2].clone() } else { String::new() };
    let table: [(&str, fn() -> f64); 9] = [
        ("random", random), ("few", few), ("nearly", nearly), ("saw", saw), ("pipe", pipe),
        ("small32", small32), ("reals", reals), ("tiny", tiny), ("keyed", keyed),
    ];
    for (name, f) in table {
        if !only.is_empty() && only != name { continue; }
        let t0 = Instant::now();
        let checksum = f();
        println!("bench\t{}\t{}\t{}", name, t0.elapsed().as_secs_f64(), checksum);
    }
}
