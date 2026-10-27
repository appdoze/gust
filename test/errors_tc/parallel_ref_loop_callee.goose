// error: a parallel for's body hands for writing ps, which another iteration may also reach: it may write the enclosing code's arrays only at its own index, as ps[i]
// Handed on from a by-reference loop to a callee that writes it.
struct P { x: i64 }
fn clear(p: P&) { p.x = 0; }
fn reset(ps: P[:]) {
    for &p in ps { clear(p); }
}
fn main() {
    var ps: P[4] = [P { 1 }, P { 2 }, P { 3 }, P { 4 }];
    var out: i64[4] = [0, 0, 0, 0];
    parallel for i in 4 {
        reset(ps);
        out[i] = i;
    }
}
