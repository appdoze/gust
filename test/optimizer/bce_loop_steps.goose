// Steps one way inside a loop: a variable every write of which inside the
// loop is a shift up (or down) that cannot wrap is, wherever the loop
// leaves it, at least (at most) what it was on entry, even where the body
// as a whole moves it both ways. A partition raises sp only inside its loop
// and lowers it once after: sp >= lo + 1 holds in the loop and after it,
// and a second recording round proves sp <= a.len from ep <= a.len. (`lo <
// hi` bounds lo above, without which `lo + 1` might wrap.)
import std;

struct Rec { key: i64, id: i64 }

fn partition(a: Rec[:], lo: i64, hi: i64) -> i64 {
    assert(lo >= 0 && lo < hi && hi <= a.len && lo + 1 < hi);
    let pivot = a[lo]; // bce:elide
    var sp = lo + 1;
    var ep = hi;
    while sp < ep {
        let c = a[sp]; // bce:elide
        if c.key < pivot.key {
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
    return sp;
}

// Lowered twice after the loop, sp can be lo - 1, below zero for lo 0.
fn too_far(a: Rec[:], lo: i64, hi: i64) -> i64 {
    assert(lo >= 0 && lo < hi && hi <= a.len && lo + 1 < hi);
    var sp = lo + 1;
    while sp < hi && a[sp].key < a[lo].key { sp += 1; }
    sp -= 2;
    var key = 0;
    if sp >= 0 { key = a[sp].key; } // bce:elide
    let after = a[sp + 1].key; // bce:elide
    return key + after + a[sp].key; // bce:keep
}

// Set as well as stepped inside the loop: no bound from its entry value.
fn reset(a: i64[:]) -> i64 {
    var i = 1;
    var acc = 0;
    var n = 0;
    while n < a.len {
        acc += a[n]; // bce:elide
        if acc > 100 { i = 0; } else { i += 1; }
        n += 1;
    }
    i -= 1;
    if i < a.len { acc += a[i]; } // bce:keep
    return acc;
}

fn main() {
    var xs: Rec[>..] = [];
    for k in 10 { xs.push(Rec { key: (k * 7) % 10, id: k }); }
    // Starts the calls do not state, so that the checks stand on the
    // functions' own facts, not their call sites'.
    let starts: i64[>..] = [0, 2];
    let p = partition(xs, starts[0], xs.len);
    print("pivot at ", p, ", key ", xs[p].key);
    print(too_far(xs, starts[1], xs.len));
    let ys: i64[>..] = [5, 6, 7];
    print(reset(ys));
}
