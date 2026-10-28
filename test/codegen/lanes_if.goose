// Branches in a loop run in lanes (src/codegen_lanes.h). A condition every
// lane shares is an `if` as it is; one that differs by lane runs both
// branches in every lane, each lane keeping its own branch's values. The
// output is the same to the last digit as one iteration at a time: TinyCC,
// MSVC and gcc run this so, clang in lanes.
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

fn main() {
    var rnd = Rnd { 11 };
    var bodies: Body[>..] = [];
    for i in 29 {
        bodies.push(Body { p: double3 { rnd.uniform(-3.0, 3.0), rnd.uniform(-3.0, 3.0), rnd.uniform(-3.0, 3.0) },
                           m: rnd.uniform(0.5, 1.5) });
    }

    // A softened force: a branch on each pair's distance, an `else if`
    // chain, and a condition every lane shares (the inner index).
    var acc: double3[>..] = [];
    acc.push_n(double3_0, bodies.len);
    for bi, i in bodies {
        var a = double3_0;
        var near = 0.0;
        for bj, j in bodies {
            let d = bj.p - bi.p;
            let r2 = dot(d, d);
            var f = 0.0;
            if r2 < 0.25 {
                f = bj.m * 4.0;
                near += 1.0;
            } else if r2 < 4.0 {
                f = bj.m / (r2 * sqrt(r2));
            } else {
                f = bj.m / (r2 * r2);
            }
            if j % 3 == 0 { f = f * 0.5; }
            a = a + d * f;
        }
        acc[i] = a + double3 { near, 0.0, 0.0 };
    }
    var sx = 0.0;
    var sz = 0.0;
    for a in acc { sx += a.x; sz += a.z; }
    print(sx, " ", sz, " ", acc[0].x, " ", acc[28].y);

    // A value `if`, conditions joined by && and || and turned by !, and
    // one branch inside another.
    var xs: f64[>..] = [];
    for i in 19 { xs.push(rnd.uniform(-2.0, 2.0)); }
    var out: f64[>..] = [];
    out.push_n(0.0, xs.len);
    for x, i in xs {
        var s = 0.0;
        for k in 1..6 {
            let y = x * k - 1.5;
            let ay = if y < 0.0 { -y } else { y };
            if ay > 1.0 && !(y > 4.0) {
                if x < 0.0 || k == 2 { s = s + sqrt(ay); } else { s = s - ay; }
            } else {
                s = s + ay * 0.25;
            }
        }
        out[i] = s;
    }
    var t = 0.0;
    for o in out { t += o; }
    print(t, " ", out[0], " ", out[18]);
}
