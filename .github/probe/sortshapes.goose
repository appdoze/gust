// Sorting in several shapes, against sortshapes.rs: the numbers are the same in both.
import std;
import os;

struct Rnd { state: u32 }

fn next(r: Rnd&) -> u32 {
    r.state = r.state * 1664525 + 1013904223;
    return r.state;
}

struct Pair { k: i64, v: i64 }

let N = 3000000;
let M = 1000000;

fn random() -> f64 {
    var r = Rnd { 7 };
    var xs: i64[>..] = [];
    for i in N { xs.push(r.next()); }
    sort(xs);
    return xs[0] + xs[xs.len / 2];
}

fn few() -> f64 {
    var r = Rnd { 7 };
    var xs: i64[>..] = [];
    for i in N { xs.push(r.next() % 1000); }
    sort(xs);
    return xs[0] + xs[xs.len / 2];
}

fn nearly() -> f64 {
    var r = Rnd { 7 };
    var xs: i64[>..] = [];
    for i in N { xs.push(if i % 100 == 0 { r.next() as i64 } else { i }); }
    sort(xs);
    return xs[0] + xs[xs.len / 2];
}

fn saw() -> f64 {
    var xs: i64[>..] = [];
    for i in N { xs.push(i % 1024); }
    sort(xs);
    return xs[0] + xs[xs.len / 2];
}

fn pipe() -> f64 {
    var xs: i64[>..] = [];
    for i in N { xs.push(if i < N / 2 { i } else { N - i }); }
    sort(xs);
    return xs[0] + xs[xs.len / 2];
}

fn small32() -> f64 {
    var r = Rnd { 7 };
    var xs: i32[>..] = [];
    for i in N { xs.push((r.next() % 2000000000) as i32); }
    sort(xs);
    return xs[0] as f64 + xs[xs.len / 2] as f64;
}

fn reals() -> f64 {
    var r = Rnd { 7 };
    var xs: f64[>..] = [];
    for i in N { xs.push(r.next() as f64); }
    sort(xs);
    return xs[0] + xs[xs.len / 2];
}

fn tiny() -> f64 {
    var r = Rnd { 7 };
    var xs: i64[>..] = [];
    for i in 64 { xs.push(0); }
    var sum = 0;
    for round in 300000 {
        let n = 2 + round % 60;
        for i in n { xs[i] = r.next(); }
        sort(xs[..n]);
        sum += xs[n / 2] & 1;
    }
    return sum;
}

fn keyed() -> f64 {
    var r = Rnd { 7 };
    var xs: Pair[>..] = [];
    for i in M { xs.push(Pair { r.next(), i }); }
    sort(xs) { a, b => a.k < b.k };
    return xs[0].k + xs[xs.len / 2].k;
}

fn run<F>(name: u8[:], only: u8[:]) {
    if only.len > 0 && only != name { return; }
    let t0 = clock();
    let checksum = F();
    print("bench\t", name, "\t", clock() - t0, "\t", checksum);
}

fn main() {
    let a = args();
    var only = "";
    if a.len > 2 { only = a[2]; }
    run("random", only, random);
    run("few", only, few);
    run("nearly", only, nearly);
    run("saw", only, saw);
    run("pipe", only, pipe);
    run("small32", only, small32);
    run("reals", only, reals);
    run("tiny", only, tiny);
    run("keyed", only, keyed);
}
