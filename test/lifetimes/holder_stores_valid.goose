// Gust: the stores the rust-lang/rust#164250 rejection fixtures
// (errors_tc/covariant_store_*) must keep allowing. A holder may take a
// reference whose root outlives it or dies with it, through every channel
// those fixtures close: a field, a whole literal, a callee, a generic, swap,
// an array, copy, a nested holder, a block and a specialization reused with
// a second valid root.
import std;

struct Q { r: i64& }
struct O { r: i64? }
struct G<T> { r: T? }
struct In { r: i64? }
struct Out { a: In, n: i64 }

fn set(q: O&, x: i64&) { q.r .= x; }
fn set_g<T>(q: G<T>&, x: T&) { q.r .= x; }
fn each<F>(n: i64) { for i in n { F(i); } }

fn main() {
    var g = 5;
    var h = 6;
    // A holder declared before its target in the same scope: both die at
    // the same exit, which frees them together.
    var o = O { r: null };
    var l = 1;
    o.r .= l;
    if o.r { print("same scope ", o.r); }
    // A field, a callee and a generic, each storing an outer reference.
    set(o, g);
    if o.r { print("callee ", o.r); }
    var gq = G<i64> { r: null };
    set_g(gq, h);
    if gq.r { print("generic ", gq.r); }
    // The same setter specialized for a second root that also lives long enough.
    set(o, h);
    if o.r { print("second root ", o.r); }
    // Inner holders pointing outward, swapped, unified in an array and copied.
    var a = Q { r: g };
    {
        var b = Q { r: h };
        swap(a, b);
        print("swap ", a.r, " ", b.r);
        var arr: Q[2] = [Q { r: g }, Q { r: h }];
        a = arr[0];
        let c = copy(arr[1]);
        print("array ", a.r, " copy ", c.r);
    }
    // A holder in a holder, written through two levels.
    var w = Out { a: In { r: null }, n: 0 };
    w.a.r .= g;
    let wr = w.a.r;
    if wr { print("nested ", wr); }
    // A block storing an outer reference into an outer holder.
    var bq = O { r: null };
    each(2) { bq.r .= g; };
    if bq.r { print("block ", bq.r); }
    // Writes through the holders reach the targets.
    a.r = 50;
    print("through ", g);
}
