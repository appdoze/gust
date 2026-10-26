// A function that calls itself enters with what its call sites prove about
// its parameters when its own calls prove it too, granted at entry: by
// induction on the depth of recursion, it then holds at every call. Here
// main passes hi == a.len, and both recursive calls pass at most hi, so
// hi <= a.len inside every call and the partition loop's checks go. The
// first check of each call, a[lo], stays: nothing relates lo to hi before
// it.
import std;

struct Rec { key: f64, id: i64 }

recursive fn quicksort<F>(a: Rec[:], lo: i64, hi: i64) {
    if hi - lo <= 1 { return; }
    let pivot = a[lo]; // bce:keep
    var sp = lo + 1;
    var ep = hi;
    while sp < ep {
        let c = a[sp]; // bce:elide
        if F(c, pivot) {
            a[sp - 1] = a[sp]; // bce:elide
            sp += 1;
        } else {
            ep -= 1;
            a[sp] = a[ep]; // bce:elide
            a[ep] = c; // bce:elide
        }
    }
    sp -= 1;
    a[sp] = pivot; // bce:elide
    quicksort(a, lo, sp, F);
    quicksort(a, ep, hi, F);
}

// One of its own calls could pass hi + 1 (it never does here, but nothing
// shows that): the assumption fails at that site, so it enters with nothing
// and the loop keeps its checks.
recursive fn widening(a: i64[:], lo: i64, hi: i64, wide: bool) -> i64 {
    if hi - lo <= 1 { return 0; }
    var acc = 0;
    var i = lo + 1;
    while i < hi {
        acc += a[i]; // bce:keep
        i += 1;
    }
    if wide { return acc + widening(a, lo + 1, hi + 1, wide); }
    return acc + widening(a, lo + 1, hi, wide);
}

// Calls from a second function into a cycle are not its own: no induction.
recursive fn ping(a: i64[:], hi: i64) -> i64 {
    if hi <= 0 { return 0; }
    return a[hi - 1] + pong(a, hi - 1); // bce:keep
}
recursive fn pong(a: i64[:], hi: i64) -> i64 { ping(a, hi) }

fn main() {
    var xs: Rec[>..] = [];
    for k in 50 { xs.push(Rec { key: (k * 37) % 50, id: k }); }
    quicksort(xs, 0, xs.len) { a, b => a.key < b.key };
    var sorted = true;
    for i in 1..xs.len { if xs[i].key < xs[i - 1].key { sorted = false; } }
    print("sorted ", sorted, ", first ", xs[0].id, ", last ", xs[xs.len - 1].id);
    let ys: i64[>..] = [1, 2, 3, 4, 5];
    print(widening(ys, 0, ys.len, false));
    print(ping(ys, ys.len));
}
