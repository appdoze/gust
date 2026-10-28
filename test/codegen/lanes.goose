// Loops whose iterations are independent run four at a time, one per lane
// (src/codegen_lanes.h), where clang builds the C; elsewhere one at a
// time. Every lane computes what its iteration does, so the output is the
// same to the last digit either way: the suite builds this with TinyCC,
// MSVC and gcc, which run it one iteration at a time, and with clang, which
// runs the lanes.
import std;
import math;
import vec;

struct Rnd { state: u32 }
fn next(r: Rnd&) -> u32 {
    r.state = r.state * 1664525 + 1013904223;
    return r.state;
}
fn uniform(r: Rnd&, a: f64, b: f64) -> f64 { a + (r.next() * (1.0 / 4294967296.0)) * (b - a) }

struct Body { p: double3, m: f64 }

// n-body's force loop: a gather of each lane's body, an inner loop every
// lane runs over the same bodies, sqrt, and a store at the lane's index.
// 37 bodies: nine blocks of four, and one body left over.
fn forces(bodies: Body[:], acc: double3[>..]&) {
    for bi, i in bodies {
        let pi = bi.p;
        var a = double3_0;
        for bj in bodies {
            let d = bj.p - pi;
            let r2 = dot(d, d) + 0.25;
            a = a + d * (bj.m / (r2 * sqrt(r2)));
        }
        acc[i] = a;
    }
}

fn main() {
    var rnd = Rnd { 7 };
    var bodies: Body[>..] = [];
    for i in 37 {
        bodies.push(Body { p: double3 { rnd.uniform(-5.0, 5.0), rnd.uniform(-5.0, 5.0), rnd.uniform(-5.0, 5.0) },
                           m: rnd.uniform(0.5, 1.5) });
    }
    var acc: double3[>..] = [];
    acc.push_n(double3_0, bodies.len);
    forces(bodies, acc);
    var sx = 0.0;
    var sy = 0.0;
    for a in acc { sx += a.x; sy += a.y * a.z; }
    print(sx, " ", sy, " ", acc[0].x, " ", acc[36].z);

    // Doubles alone: a load at the lane's index from a second array, an
    // inner range loop, a load at an index every lane shares, negation.
    var xs: f64[>..] = [];
    var ws: f64[>..] = [];
    for i in 23 { xs.push(rnd.uniform(-1.0, 1.0)); ws.push(rnd.uniform(0.0, 2.0)); }
    var out: f64[>..] = [];
    out.push_n(0.0, 23);
    for x, i in xs {
        var s = -x;
        for k in 1..9 {
            s = s * 0.5 + ws[k] / (x * x + k);
        }
        out[i] = s - ws[i];
    }
    var total = 0.0;
    for o in out { total += o; }
    print(total, " ", out[0], " ", out[22]);

    // A body with a call to anything but sqrt runs one iteration at a time.
    var plain: f64[>..] = [];
    plain.push_n(0.0, 9);
    for x, i in xs {
        if i >= 9 { break; }
        var s = 0.0;
        for k in 3 { s += abs(x) * k; }
        plain[i] = s;
    }
    print(plain[0], " ", plain[8]);
}
