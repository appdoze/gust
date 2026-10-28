// Gust: the lanes' filter form (src/codegen_lanes.h): a loop of pure
// statements ending in an if tests four iterations at once, and runs the
// four as the loop does wherever any lane's tests hold. Every result is the
// loop's one iteration at a time: the ifs' effects in order, a search's
// first hit, an argmin's ties, a condition on state the if changes, an
// element the if writes before the lanes would have read it, break and
// continue, NaNs, and lengths that leave a remainder.
import std;
import math;
import vec;

struct Ball { c: double3, r: f64, w: f64 }

// Early returns, as a ray tracer's sphere test has them.
fn hit(b: Ball&, o: double3, d: double3) -> f64 {
    let oc = b.c - o;
    let p = dot(oc, d);
    let q = dot(oc, oc) - b.r * b.r;
    let disc = p * p - q;
    guard disc > 0.0 else { return -1.0; }
    let s = sqrt(disc);
    let t = p - s;
    if t > 0.001 { return t; }
    let t2 = p + s;
    return if t2 > 0.001 { t2 } else { -1.0 };
}

fn any(balls: Ball[:], o: double3, d: double3) -> bool {
    for &b in balls {
        if hit(b, o, d) > 0.0 { return true; }
    }
    return false;
}

// The first of the nearest, its index and how many hits came before.
var seen = 0;
fn nearest(balls: Ball[:], o: double3, d: double3) -> f64, i64 {
    var best = -1.0;
    var at = -1;
    for &b, i in balls {
        let t = hit(b, o, d);
        if t > 0.0 && (best < 0.0 || t < best) {
            best = t;
            at = i;
            seen += 1;
        }
    }
    return best, at;
}

// The if writes the next element, which the lanes read before it did.
fn chain(balls: Ball[>..]&) -> i64 {
    var n = 0;
    for &b, i in balls {
        let w = b.w * 2.0;
        if w > 1.0 {
            n += 1;
            if i + 1 < balls.len { balls[i + 1].w = 0.75; }
        }
    }
    return n;
}

// Break and continue in the if.
fn first_two(xs: f64[:]) -> i64 {
    var found = 0;
    var sum = 0;
    for x, i in xs {
        let y = x * x - 2.0;
        if y > 0.0 {
            if i % 3 == 0 { continue; }
            sum += i;
            found += 1;
            if found == 2 { break; }
        }
    }
    return sum;
}

fn main() {
    var balls: Ball[>..] = [];
    for i in 23 {
        balls.push(Ball { c: double3 { x: i * 0.37 - 4.0, y: (i % 5) * 0.5, z: 6.0 + (i % 7) },
                          r: 0.3 + (i % 4) * 0.1, w: (i % 6) * 0.2 });
    }
    // Two the same, so the first of them wins.
    balls.push(balls[9]);
    let o = double3 { x: 0.0, y: 0.5, z: 0.0 };
    var hits = 0;
    var total = 0.0;
    var where = 0;
    for k in 41 {
        let d = normalize(double3 { x: (k - 20.0) * 0.05, y: 0.02 * (k % 3), z: 1.0 });
        if any(balls, o, d) { hits += 1; }
        let t, at = nearest(balls, o, d);
        total += t;
        where += at * (k + 1);
    }
    print("hits ", hits, ", nearest ", total, ", where ", where, ", seen ", seen);
    let short = balls[0..6];
    let st, sat = nearest(short, o, double3 { x: 0.0, y: 0.0, z: 1.0 });
    print("six: ", any(short, o, double3 { x: -0.6, y: 0.0, z: 1.0 }), " ", st, " at ", sat);
    print("chain ", chain(balls), " then ", balls[23].w);
    var xs: f64[>..] = [];
    for i in 19 { xs.push(if i == 4 { 0.0 / 0.0 } else { i * 0.4 }); }
    print("first two ", first_two(xs));
}
