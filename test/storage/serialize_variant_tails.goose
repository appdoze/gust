// A fixed-mode ADT value is as large as its largest variant: the bytes a
// smaller variant does not use (all of them, for one without fields) are
// zero however the value was built, so that to_bytes, which writes the
// value's bytes as they are, never writes out what the stack held before.
// Each value below is built after a call that leaves the stack full of
// 0x5A bytes.

enum Shape { Circle { r: f32 }, Rect { w: f32, h: f32 }, Dot }

fn noise() -> i64 {
    var a: i64[32] = [0x5A5A5A5A5A5A5A5A; 32];
    var s = 0;
    for x, i in a { s += x >> (i & 7); }
    return s & 1;
}

fn make(k: i64) -> Shape {
    if k == 0 { return Shape.Dot {}; }
    if k == 1 { return Shape.Circle { r: 2.0 }; }
    return Shape.Rect { w: 1.0, h: 1.0 };
}

fn show(what: const u8[:], pool: Shape[>..]&) {
    var line: u8[>..] = [];
    for b in pool.to_bytes() { format(line, b, " "); }
    print(what, ": ", line);
}

fn main() {
    var pool: Shape[>..] = [];
    var n = noise();
    pool.push(Shape.Dot {});
    pool.push(Shape.Circle { r: 1.0 });
    show("literals", pool);

    pool.clear();
    n += noise();
    let d = Shape.Dot {};
    let c = Shape.Circle { r: 1.0 };
    pool.push(d);
    pool.push(c);
    show("variant values", pool);

    pool.clear();
    for k in 3 {
        n += noise();
        pool.push(make(k));
    }
    show("returned", pool);

    pool.clear();
    n += noise();
    var s: Shape = Shape.Rect { w: 3.0, h: 4.0 };
    n += noise();
    s = Shape.Dot {};
    pool.push(s);
    show("assigned", pool);
    print(n >= 0);
}
