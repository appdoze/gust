// An array is of one type, and a reference reaches only an array of its
// own type, so a growth or shrink through one, or a callee's through an
// argument holding scalars, leaves the lengths of other types' arrays alone
// (§10.5). Same-typed arrays may be one, and keep their checks: in a body
// kept out of line (`recursive fn`), since inlined into main each call binds
// its parameters to two variables the pass tells apart.

import std;

recursive fn same_type(a: i32[>..<]&, b: i32[>..<]&) -> i32 {
    guard b.len > 0 else { return 0; }
    a.clear();
    return b[0];                               // bce:keep (a may be b)
}

fn other_type(a: u32[>..<]&, b: i32[>..<]&) -> i32 {
    guard b.len > 0 else { return 0; }
    a.clear();
    return b[0];                               // bce:elide (a u32 array is not b)
}

fn other_kind(a: i32[>..<]&, b: i32[>..]&) -> i32 {
    guard b.len > 0 else { return 0; }
    a.clear();
    return b[0];                               // bce:elide (a grow-shrink array is not b)
}

fn drain(a: i32[>..<]&) { a.clear(); }
fn drain_u(a: u32[>..<]&) { a.clear(); }

recursive fn callee_same(a: i32[>..<]&, b: i32[>..<]&) -> i32 {
    guard b.len > 0 else { return 0; }
    drain(a);
    return b[0];                               // bce:keep (a may be b)
}

fn callee_other(a: u32[>..<]&, b: i32[>..<]&) -> i32 {
    guard b.len > 0 else { return 0; }
    drain_u(a);
    return b[0];                               // bce:elide
}

fn fresh(v: i32) -> i32[>..<] {
    var x: i32[>..<] = [];
    x.push(v);
    return x;
}

fn main() {
    var u: u32[>..<] = [];
    u.push(1);
    var g: i32[>..] = [];
    g.push(8);
    var x = fresh(7);
    print(other_type(u, x), " ", callee_other(u, x));
    var y = fresh(9);
    print(other_kind(y, g));
    var p = fresh(1);
    var q = fresh(2);
    var r = fresh(3);
    print(same_type(p, q), " ", callee_same(r, q));
}
