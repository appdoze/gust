// Gust: `parallel for` (§6.5). Iterations run on workers in a build and in
// order in a JIT run, and the output is the same either way: each writes
// only its own element of arrays the enclosing code owns, reads what no
// iteration writes, and keeps its own storage on its own thread's stacks.

import std;

struct P { x: f64, y: f64, hits: i64 }

let SCALE = 3;
var table: i64[>..] = [];

fn mix(a: i64, b: i64) -> i64 { (a * 31 + b * 17) % 1009 }
// It takes the slice for writing, and only reads it: a body may hand it the
// enclosing code's storage, and recursion, which needs the fixpoint, too.
recursive fn total_of(xs: i64[:], from: i64) -> i64 {
    if from >= xs.len { return 0; }
    return xs[from] + total_of(xs, from + 1);
}

// It walks what it is handed by reference, as a tracer walks its spheres,
// and writes nothing through it.
fn farthest(ps: P[:], from: f64) -> f64 {
    var m = 0.0;
    for &p in ps { m = max(m, max(p.x - from, from - p.x)); }
    return m;
}

fn main() {
    for k in 64 { table.push(k * k % 97); }
    let n = 200000;

    // Its own element of an owned array, from what nothing writes: a global,
    // a local of the enclosing code, a helper.
    var out: i64[>..] = [];
    out.push_n(0, n);
    let bias = 5;
    parallel for i in n {
        out[i] = mix(i, bias) * SCALE + table[i % 64];
    }
    var t = 0;
    for x in out { t += x; }
    print("own elements ", t, " ", out[0], " ", out[n - 1]);

    // Storage of its own, on its thread's stacks, and a nested loop with a
    // break of its own.
    var lens: i64[>..] = [];
    lens.push_n(0, n);
    parallel for i in n {
        var digits: i64[>..] = [];
        var v = i;
        loop {
            digits.push(v % 10);
            v = v / 10;
            if v == 0 { break; }
        }
        lens[i] = digits.len * 100 + digits[digits.len - 1];
    }
    var lt = 0;
    for x in lens { lt += x; }
    print("own storage ", lt, " ", lens[7], " ", lens[123456]);

    // A field of its own element, floats, and a continue that ends an
    // iteration early.
    var pts: P[>..] = [];
    for i in 1000 { pts.push(P { x: i * 0.5, y: 0.0, hits: 0 }); }
    parallel for i in pts.len {
        if i % 3 == 0 { continue; }
        pts[i].y = pts[i].x * pts[i].x;
        pts[i].hits = i % 7;
    }
    var ys = 0.0;
    var hs = 0;
    for p in pts { ys += p.y; hs += p.hits; }
    print("fields ", ys, " ", hs, " ", pts[2].y, " ", pts[3].y);

    // A range, and ranges with no iteration or one.
    var r: i64[>..] = [];
    r.push_n(-1, 30);
    parallel for i in 10..20 { r[i] = i * i; }
    let none = 0;
    parallel for i in none { r[i] = 999; }
    parallel for i in 25..26 { r[i] = 1; }
    print("ranges ", r[9], " ", r[10], " ", r[19], " ", r[20], " ", r[25]);

    // A callee that only reads what it is handed for writing.
    var sums: i64[>..] = [];
    sums.push_n(0, 100);
    parallel for i in 100 { sums[i] = total_of(table, i % 64) + i; }
    print("read-only callee ", sums[0], " ", sums[63], " ", sums[99]);

    // A callee reading the enclosing code's storage through references.
    var far: f64[>..] = [];
    far.push_n(0.0, 50);
    parallel for i in 50 { far[i] = farthest(pts, i * 1.0); }
    print("by-reference reads ", far[0], " ", far[49]);
}
