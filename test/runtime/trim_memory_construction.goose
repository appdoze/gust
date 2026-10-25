// runtime-debug
// trim_memory called while a statement is still building values: an argument
// built before a later one's call, a literal's elements and fields, a builder
// between the pieces str() appends, a loop's pushes and global initializers.
// Whatever has been built so far lies below a top the call has written back.
import os;

// Out of line even at -O2, so that their callers keep their tops cached.
recursive fn trim_at(depth: i64) -> bool {
    if depth > 0 { return trim_at(depth - 1); }
    return trim_memory();
}
recursive fn trimmed_three(depth: i64) -> i64 {
    if depth > 0 { return trimmed_three(depth - 1); }
    assert(trim_memory());
    return 3;
}
recursive fn row(depth: i64, n: i64, v: u8) -> u8[] {
    if depth > 0 { return row(depth - 1, n, v); }
    assert(trim_memory());
    var r: u8[>..] = [];
    r.resize(n, v);
    return copy(r);
}

fn filled(xs: const u8[:], v: u8) -> bool {
    for c in xs { if c != v { return false; } }
    return true;
}

fn make(n: i64, v: u8) -> u8[>..] {
    var r: u8[>..] = [];
    r.resize(n, v);
    assert(trim_at(1));
    return r;
}
fn measure(a: u8[>..], k: i64, b: u8[>..]) -> i64 {
    assert(filled(a, 99) && filled(b, 98));
    return a.len + k + b.len;
}

struct Doc { id: i64, body: u8[] }

// The second initializer's trim keeps the first global.
var first: u8[>..] = make(1024 * 1024, 31);
var second: u8[>..] = make(1024 * 1024, 32);

fn main() {
    assert(filled(first, 31) && filled(second, 32));

    var other: u8[>..] = [];
    other.resize(1024 * 1024, 98);
    let total = measure(make(2 * 1024 * 1024, 99), trimmed_three(1), copy(other));
    assert(total == 3 * 1024 * 1024 + 3);

    let rows: u8[][>..] = [row(1, 1024 * 1024, 1), row(1, 1024 * 1024, 2), row(1, 5000, 3)];
    var k: u8 = 0;
    for r in rows {
        k += 1;
        assert(filled(r, k));
    }
    assert(k == 3);
    let d = Doc { trimmed_three(1), row(1, 2 * 1024 * 1024, 9) };
    assert(d.id == 3 && filled(d.body, 9));

    var big: u8[>..] = [];
    big.resize(1024 * 1024, 88);
    let s = str(big, trimmed_three(1), big);
    assert(s.len == 2 * 1024 * 1024 + 1 && s[1024 * 1024] == 51);
    assert(filled(s[..1024 * 1024], 88) && filled(s[1024 * 1024 + 1..], 88));

    var data: i64[>..] = [];
    for i in 600000 {
        data.push(i);
        if i % 100000 == 0 { assert(trim_at(1)); }
    }
    for i in data.len { assert(data[i] == i); }

    print("trim memory keeps what a statement has built so far");
}
