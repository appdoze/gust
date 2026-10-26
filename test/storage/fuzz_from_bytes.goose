// Fuzzing from_bytes (spec §12) with the images an attacker could send:
// valid images of pools of each shape the serializer has (relative
// references of every width, optional and not, cycles, a reference typed at
// one variant, fixed-mode and variable-mode enums, varints, bools, length-
// prefixed and capacity arrays, a pad), mutated at random: bits flipped,
// bytes set to boundary values, nudged, cut, inserted, deleted, and runs
// copied over others. Whatever from_bytes accepts is walked in full, every
// reference followed into its target, and must serialize to an image it
// accepts again with the same digest. Under ASan and UBSan
// (test/fuzz_untrusted.py runs this for millions of images) an image the
// verifier wrongly let through shows as an access out of bounds.
//
//   fuzz_from_bytes [images per shape] [seed]

import os;

struct Rnd { s: u64 }
fn next(r: Rnd&) -> u64 {
    r.s ^= r.s << 13;
    r.s ^= r.s >> 7;
    r.s ^= r.s << 17;
    return r.s;
}
fn below(r: Rnd&, n: i64) -> i64 { ((r.next() >> 1) as i64) % n }
fn byte(r: Rnd&) -> u8 { (r.next() >> 56) as u8 }

// A copy of img with one to three random changes.
fn mutate(img: u8[:], out: u8[>..]&, r: Rnd&) {
    out.clear();
    out.append(img);
    for step in 1 + r.below(3) {
        let kind = r.below(8);
        let n = out.len;
        if n == 0 {
            out.push(r.byte());
            continue;
        }
        let at = r.below(n);
        if kind == 0 {
            out[at] ^= (1 << r.below(8)) as u8;
        } else if kind == 1 {
            let edges: u8[6] = [0, 1, 127, 128, 254, 255];
            out[at] = edges[r.below(6)];
        } else if kind == 2 {
            out[at] = ((out[at] as i64 + 1 + 254 * r.below(2)) & 255) as u8;   // one up or down, wrapping
        } else if kind == 3 {
            out.resize(r.below(n + 1), 0);
        } else if kind == 4 {
            out.push(0);
            var i = n;
            while i > at { out[i] = out[i - 1]; i -= 1; }
            out[at] = r.byte();
        } else if kind == 5 {
            for i in at..n - 1 { out[i] = out[i + 1]; }
            out.resize(n - 1, 0);
        } else if kind == 6 {
            // A run copied over another: offsets and lengths from elsewhere.
            let len = 1 + r.below(4);
            let from = r.below(n);
            for i in len {
                if at + i < n && from + i < n { out[at + i] = out[from + i]; }
            }
        } else {
            out[at] = r.byte();
        }
    }
}

struct Tally { tried: i64 = 0, verified: i64 = 0, same: i64 = 0 }

// The trial loop, for any element type T and its digest D, which must touch
// everything a verified pool holds.
fn fuzz<T, D>(name: const u8[:], pool: T[>..]&, n: i64, r: Rnd&) {
    let img = pool.to_bytes();
    let base, base_ok = from_bytes<T[>..]>(img);
    assert(base_ok && D(base) == D(pool));
    var t = Tally { .. };
    var m: u8[>..] = [];
    for k in n {
        mutate(img, m, r);
        t.tried += 1;
        let back, ok = from_bytes<T[>..]>(m);
        if !ok {
            assert(back.len == 0);
            continue;
        }
        t.verified += 1;
        let d = D(back);
        let again = back.to_bytes();
        let back2, ok2 = from_bytes<T[>..]>(again);
        assert(ok2 && D(back2) == d);
        if again == m { t.same += 1; }
    }
    print(name, ": ", t.tried, " images, ", t.verified, " accepted, ", t.same,
          " of those as they serialize again");
}

// A digest that cannot overflow, which --checked would abort on.
fn mix(acc: i64, x: i64) -> i64 { (acc * 31 + x % 1000003) % 1000000007 }
fn fbits(x: f64) -> i64 { (if x > 0.0 { 1 } else { 0 }) + (if x < 0.0 { 2 } else { 0 }) + (if x != x { 4 } else { 0 }) }

struct Tree { v: i32, l: Tree&<u32>?, r: Tree&<u32>? }
fn tree_digest(a: Tree[>..]&) -> i64 {
    var acc = 0;
    for &n in a {
        acc = mix(acc, n.v);
        let l = n.l;
        if l { acc = mix(acc, l.v); let ll = l.l; if ll { acc = mix(acc, ll.v); } }
        let r = n.r;
        if r { acc = mix(acc, r.v); }
    }
    return acc;
}

struct Ring { v: u8, prev: Ring&<u16>, next: Ring&<u16> }
fn ring_digest(a: Ring[>..]&) -> i64 {
    var acc = 0;
    for &n in a { acc = mix(mix(mix(acc, n.v), n.prev.v), n.next.next.v); }
    return acc;
}

enum Knot { Loop { n: u8, back: Knot.Loop&<u8> }, Plain { n: u8 } }
fn knot_digest(a: Knot.Loop[>..]&) -> i64 {
    var acc = 0;
    for &k in a { acc = mix(mix(acc, k.n), k.back.n); }
    return acc;
}

enum Shape { Circle { r: f32 }, Rect { w: f32, h: f32 }, Dot }
fn shape_digest(a: Shape[>..]&) -> i64 {
    var acc = 0;
    for &s in a {
        acc = match s {
            Circle c => mix(mix(acc, 1), fbits(c.r)),
            Rect q => mix(mix(mix(acc, 2), fbits(q.w)), fbits(q.h)),
            Dot => mix(acc, 3),
        };
    }
    return acc;
}

struct Cell { k: u16, kids: (Cell&<u16>)[..3] }
fn cell_digest(a: Cell[>..]&) -> i64 {
    var acc = 0;
    for &c in a {
        acc = mix(mix(acc, c.k), c.kids.len);
        for &kid in c.kids { acc = mix(mix(acc, kid.k), kid.kids.len); }
    }
    return acc;
}

enum Sexp {
    Sym  { name: u8[varint] },
    Pair { head: Sexp..&<varint>, tail: Sexp..&<varint>?, extra: (i16)[varint] },
}
fn sexp_one(acc: i64, s: Sexp..&) -> i64 {
    var a = acc;
    match s {
        Sym &y => {
            a = mix(a, y.name.len);
            for &c in y.name { a = mix(a, c); }
        },
        Pair &p => { a = mix(mix(a, p.extra.len), 7); }
    }
    return a;
}
fn sexp_digest(a: Sexp..[>..]&) -> i64 {
    var acc = 0;
    for s in a {
        match s {
            Sym &y => { for &c in y.name { acc = mix(acc, c); } },
            Pair &p => {
                acc = sexp_one(acc, p.head);
                let t = p.tail;
                if t { acc = sexp_one(acc, t); }
                for &x in p.extra { acc = mix(acc, x); }
            }
        }
    }
    return acc;
}

enum Item {
    Word { text: u8[varint] },
    Ref  { to: Item.Word&<u16>? },
}
fn item_digest(a: Item..[>..]&) -> i64 {
    var acc = 0;
    for e in a {
        match e {
            Word &x => { for &c in x.text { acc = mix(acc, c); } },
            Ref &q => {
                let t = q.to;
                if t { acc = mix(acc, t.text.len); for &c in t.text { acc = mix(acc, c); } }
                else { acc = mix(acc, -1); }
            }
        }
    }
    return acc;
}

struct Rec {
    id: u32,
    tags: u8[],
    slots: i32[..],
    link: Rec&<u32>?,
}
fn rec_digest(a: Rec[>..]&) -> i64 {
    var acc = 0;
    for q in a {
        acc = mix(acc, q.id);
        for &c in q.tags { acc = mix(acc, c); }
        for &x in q.slots { acc = mix(acc, x); }
        let l = q.link;
        if l {
            acc = mix(mix(acc, l.id), l.tags.len);
            for &x in l.slots { acc = mix(acc, x); }
        }
    }
    return acc;
}

struct Pad { a: u8, pad, b: u32, l: Pad&<u16>? }
fn pad_digest(a: Pad[>..]&) -> i64 {
    var acc = 0;
    for &p in a {
        acc = mix(mix(acc, p.a), p.b);
        let l = p.l;
        if l { acc = mix(acc, l.b); }
    }
    return acc;
}

struct V { x: varint, b: bool, f: f64 }
fn v_digest(a: V[>..]&) -> i64 {
    var acc = 0;
    for v in a { acc = mix(mix(mix(acc, v.x), if v.b { 1 } else { 0 }), fbits(v.f)); }
    return acc;
}

fn main() {
    let a = args();
    var n = 4000;
    var seed = 1;
    if a.len > 1 { let k, ok = parse_int(a[1]); assert(ok && k > 0); n = k; }
    if a.len > 2 { let k, ok = parse_int(a[2]); assert(ok); seed = k; }
    var r = Rnd { (seed as u64) * 0x9E3779B97F4A7C15 + 1 };

    var trees: Tree[>..] = [];
    let ta: Tree& = trees.push(Tree { v: 11 });
    let tb: Tree& = trees.push(Tree { v: -22 });
    let tc: Tree& = trees.push(Tree { v: 33, l: ta, r: tb });
    trees.push(Tree { v: 44, l: tc, r: ta });
    fuzz("tree", trees, n, r, tree_digest);

    var rings: Ring[>..] = [];
    rings.push(Ring { v: 7, prev: self, next: self });
    rings.push(Ring { v: 8, prev: self, next: self });
    rings.push(Ring { v: 9, prev: self, next: self });
    fuzz("ring", rings, n, r, ring_digest);

    var knots: Knot.Loop[>..] = [];
    let ka: Knot.Loop& = knots.push(Knot.Loop { n: 1, back: self });
    knots.push(Knot.Loop { n: 3, back: ka });
    fuzz("knot", knots, n, r, knot_digest);

    var shapes: Shape[>..] = [];
    shapes.push(Shape.Circle { r: 1.0 });
    shapes.push(Shape.Rect { w: 2.0, h: -3.0 });
    shapes.push(Shape.Dot {});
    fuzz("shape", shapes, n, r, shape_digest);

    var cells: Cell[>..] = [];
    let ca: Cell& = cells.push(Cell { k: 1, kids: [] });
    let cb: Cell& = cells.push(Cell { k: 2, kids: [ca] });
    cells.push(Cell { k: 3, kids: [ca, cb] });
    fuzz("cell", cells, n, r, cell_digest);

    var sexps: Sexp..[>..] = [];
    let sa: Sexp..& = sexps.push(Sexp.Sym { name: "alpha" });
    let sb: Sexp..& = sexps.push(Sexp.Sym { name: "b" });
    let sp: Sexp..& = sexps.push(Sexp.Pair { head: sa, tail: sb, extra: [1, -2, 3] });
    sexps.push(Sexp.Pair { head: sp, extra: [] });
    fuzz("sexp", sexps, n, r, sexp_digest);

    var items: Item..[>..] = [];
    let w: Item..& = items.push(Item.Word { text: "hi" });
    match w {
        Word &x => { items.push(Item.Ref { to: x }); },
        Ref &_ => {}
    }
    items.push(Item.Ref { });
    fuzz("item", items, n, r, item_digest);

    var recs: Rec[>..] = [];
    let ra: Rec& = recs.push(Rec { id: 1, tags: "ab", slots: [1, 2, 3] });
    recs.push(Rec { id: 2, tags: "", slots: [], link: ra });
    fuzz("rec", recs, n, r, rec_digest);

    var pads: Pad[>..] = [];
    let pa: Pad& = pads.push(Pad { a: 1, b: 100 });
    pads.push(Pad { a: 2, b: 200, l: pa });
    fuzz("pad", pads, n, r, pad_digest);

    var vs: V[>..] = [V { 0, false, 0.5 }, V { -1, true, -2.0 }, V { 300, false, 0.0 }];
    fuzz("varint", vs, n, r, v_digest);
}
