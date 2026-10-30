// Gust: a loop index's invariant (`i <= len - 1` for a masked probe) is set
// where the index is declared and kept at each of its writes, so only a
// shrink or re-binding while the index lives can break it. A table the
// caller sized before the probe began keeps the probe's checks out, inlined
// as well; a shrink while the index lives keeps them, and the last probe,
// of a table shrunk under it, aborts.
import std;

struct Slot { key: i64, tag: u32 }
struct Table { slots: Slot[>..<] = [] }

fn find(d: Table&, key: i64, h: u64) -> i64 {
    if d.slots.len == 0 { return -1; }
    let mask = d.slots.len - 1;
    var i = (h & (mask as u64)) as i64;
    loop {
        let t = d.slots[i].tag;                                // bce:elide
        if t == 0 || d.slots[i].key == key { return i; }       // bce:elide
        i = (i + 1) & mask;
    }
}

// The table shrinks while the probe index lives.
fn find_shrinking(d: Table&, key: i64, h: u64) -> i64 {
    if d.slots.len == 0 { return -1; }
    let mask = d.slots.len - 1;
    var i = (h & (mask as u64)) as i64;
    loop {
        let t = d.slots[i].tag;                                // bce:keep
        if t == 0 || d.slots[i].key == key { return i; }
        d.slots.pop();
        if d.slots.len == 0 { return -1; }
        i = (i + 1) & mask;
    }
}

// An index declared before a loop that shrinks the array.
fn outer_index(xs: i64[>..]&, h: i64) -> i64 {
    if xs.len == 0 { return 0; }
    let mask = xs.len - 1;
    var i = h & mask;
    var s = 0;
    for k in 3 {
        s += xs[i];                                            // bce:keep
        xs.pop();
        if xs.len == 0 { break; }
        i = (i + 1) & mask;
    }
    return s;
}

// The shrink comes before the index exists.
fn shrunk_first(xs: i64[>..]&, h: i64) -> i64 {
    xs.pop();
    if xs.len == 0 { return 0; }
    let mask = xs.len - 1;
    var i = h & mask;
    var s = 0;
    for k in 4 {
        s += xs[i];                                            // bce:elide
        i = (i + 3) & mask;
    }
    return s;
}

fn main() {
    var t = Table { .. };
    t.slots.resize(8, Slot { key: 0, tag: 0 });
    t.slots[5] = Slot { key: 42, tag: 1 };
    print("find ", find(t, 42, 5), " ", find(t, 7, 5), " ", find(t, 1, 13));
    var xs: i64[>..] = [];
    for k in 8 { xs.push(k + 1); }
    print("outer ", outer_index(xs, 1), " leaves ", xs.len);
    var ys: i64[>..] = [];
    for k in 9 { ys.push(k * 10); }
    print("shrunk first ", shrunk_first(ys, 6));
    var u = Table { .. };
    for k in 8 { u.slots.push(Slot { key: 100 + k, tag: 1 }); }
    print("shrinking ", find_shrinking(u, 999, 6));
}
