// Gust: what a callee leaves of the length of an array it is handed by
// reference. A body that resizes such an array has an exit summary, found
// assuming its integer parameters nonnegative where it needs that, and a
// call site that proves the assumption gets the length the call leaves;
// inlined, a `let` reference to a variable is that variable's array.

import std;
import os;

recursive fn grow_rec(xs: i64[>..<]&, n: i64) { for i in n { xs.push(i); } }
recursive fn grow_both(xs: i64[>..<]&, n: i64) { grow_rec(xs, n); grow_rec(xs, n); }
recursive fn maybe(xs: i64[>..<]&, c: i64) { if c > 3 { xs.push(c); } }
recursive fn drop2(xs: i64[>..<]&) { xs.pop(); xs.pop(); }
recursive fn refill(xs: i64[>..<]&, n: i64) { xs.clear(); for i in n { xs.push(i); } }

fn main() {
    let k = args().len + 1;                    // At least 1, unknown here.
    var a: i64[>..<] = [];
    a.push_n(7, k);                            // std's push_n, inlined.
    print(a[k - 1]);                           // bce:elide (a.len == k)

    var b: i64[>..<] = [];
    grow_rec(b, k);
    print(b[k - 1]);                           // bce:elide (out of line: b.len == k)
    grow_both(b, k);
    print(b[k - 1]);                           // bce:elide (through grow_rec's summary)

    var c: i64[>..<] = [1, 2, 3];
    let m = k - 2;                             // May be -1: no exact length.
    grow_rec(c, m);
    if c.len > 0 { print(c[2]); }              // bce:keep (the summary needs m >= 0)

    var d: i64[>..<] = [1, 2];
    maybe(d, k);
    print(d[1]);                               // bce:elide (d.len >= 2)
    if k > 9 { print(d[2]); }                  // bce:keep (d.len may still be 2)

    var e: i64[>..<] = [1, 2, 3, 4];
    drop2(e);
    print(e[1]);                               // bce:elide (e.len == 2)

    var f: i64[>..<] = [9];
    refill(f, k + 1);
    print(f[k]);                               // bce:elide (f.len == k + 1, whatever it was)

    var g: i64[>..] = [];
    let r = g;
    for i in k { r.push(i); }
    print(g[k - 1]);                           // bce:elide (r is g)
}
