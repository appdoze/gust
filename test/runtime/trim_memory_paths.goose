// runtime-debug
// trim_memory reached in each way a call can be made while its caller has
// grown a stack whose top it keeps in a local: a static function value, a
// block that returns from its own function, map's block, a nested function,
// a reference parameter, a global, a case function and a format hook. Every
// caller up the chain writes its tops back first, or the pages above a stale
// top would be discarded.
import os;

// Out of line even at -O2, so that its callers keep their tops cached.
recursive fn trim_at(depth: i64) -> bool {
    if depth > 0 { return trim_at(depth - 1); }
    return trim_memory();
}

fn filled(xs: const u8[:], v: u8) -> bool {
    for c in xs { if c != v { return false; } }
    return true;
}

// The extern itself as a static function value.
fn apply<F>() -> bool { F() }

// A block whose `return` leaves grow_until, through the HOF's frames.
fn each_n<F>(n: i64) { for i in n { F(i); } }
fn grow_until(data: u8[>..]&, stop: i64) -> i64 {
    each_n(4) { i =>
        data.resize(data.len + 1024 * 1024, 77);
        assert(trim_at(1));
        if i == stop { return data.len; }
    }
    return -1;
}

// A stack reached through a reference parameter.
fn grow_ref(b: u8[>..]&, n: i64, v: u8) {
    b.resize(n, v);
    assert(trim_at(1));
    assert(filled(b, v));
}
var global_doc: u8[>..] = [];

enum Shape { Circle { r: f64 }, Rect { w: f64, h: f64 } }
fn visit(c: Shape.Circle) -> i64 { assert(trim_at(1)); return 1; }
fn visit(r: Shape.Rect) -> i64 { return 2; }

struct Label { n: i64 }
fn format(out: u8[>..]&, l: Label&) { assert(trim_at(1)); format(out, "#", l.n); }

fn main() {
    var a: u8[>..] = [];
    a.resize(2 * 1024 * 1024, 73);
    assert(apply(trim_memory));
    assert(filled(a, 73));

    // A nested function growing a captured array.
    var b: u8[>..] = [];
    fn grow_b(n: i64) {
        b.resize(n, 55);
        assert(trim_at(1));
        assert(filled(b, 55));
    }
    grow_b(2 * 1024 * 1024);
    b.resize(1);
    grow_b(3 * 1024 * 1024);
    assert(filled(b, 55));

    var c: u8[>..] = [];
    assert(grow_until(c, 2) == 3 * 1024 * 1024);
    assert(filled(c, 77));
    let xs = [1, 2, 3, 4];
    let doubled = map(xs) { x => assert(trim_at(1)); x * 2 };
    assert(doubled[3] == 8);

    var d: u8[>..] = [];
    grow_ref(d, 2 * 1024 * 1024, 44);
    grow_ref(global_doc, 3 * 1024 * 1024, 45);
    assert(filled(d, 44) && filled(global_doc, 45));

    // A dispatched call inside a loop that grows an array.
    var shapes: Shape[>..] = [Shape.Rect { 1.0, 2.0 }, Shape.Circle { 3.0 }];
    var e: u8[>..] = [];
    var kinds = 0;
    for s in shapes {
        e.resize(e.len + 1024 * 1024, 66);
        kinds += visit(s);
        assert(filled(e, 66));
    }
    assert(kinds == 3);

    // A format hook appending to a grown builder, and one inside str().
    var text: u8[>..] = [];
    text.resize(1024 * 1024, 65);
    let l = Label { 7 };
    format(text, l, " ", l);
    assert(text.len == 1024 * 1024 + 5);
    assert(filled(text[..1024 * 1024], 65));
    print("trim memory reaches every caller's cached tops: ", str(text.len, l));
}
