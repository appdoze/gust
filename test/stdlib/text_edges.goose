// Edges of std's text and numbers that once went wrong: UTF-8 that is not
// well-formed, padding after a sign, -0.0, and rand_int over a large range.
import std;

fn hex(s: u8[:]) -> u8[>..] {
    var out: u8[>..] = [];
    for c in s { format_uint(out, c, 16, 2, '0'); }
    return out;
}

fn utf8(name: u8[:], s: u8[:]) {
    var out: u8[>..] = [];
    each_utf8(s) {
        if out.len > 0 { out.push(' '); }
        format_int(out, it, 16, 0, ' ');
    };
    print(name, ": ", out);
}

fn pushed(cp: i64) {
    var out: u8[>..] = [];
    push_utf8(out, cp);
    print("push_utf8(", cp, ") = ", hex(out));
}

fn padded(v: i64, width: i64, padc: u8) {
    var out: u8[>..] = [];
    format_int(out, v, 10, width, padc);
    print("\"", out, "\"");
}

fn main() {
    utf8("every length", "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
    utf8("first and last of each", "\x7F\xC2\x80\xDF\xBF\xE0\xA0\x80\xEF\xBF\xBF\xF0\x90\x80\x80\xF4\x8F\xBF\xBF");
    utf8("around the surrogates", "\xED\x9F\xBF\xEE\x80\x80");
    utf8("lone byte", "\xFF");
    utf8("continuation alone", "\x80");
    utf8("overlong", "\xC0\x80");
    utf8("overlong 2", "\xC1\xBF");
    utf8("overlong 3", "\xE0\x9F\xBF");
    utf8("overlong 4", "\xF0\x8F\xBF\xBF");
    utf8("surrogate", "\xED\xA0\x80");
    utf8("past the end", "\xF4\x90\x80\x80");
    utf8("past the leads", "\xF5\x80\x80\x80");
    utf8("cut short", "\xE2\x82");
    utf8("cut short 4", "\xF0\x9F\x98");
    utf8("swallows nothing", "a\xE2\x82b");
    utf8("swallows no quote", "\xC3\"x");

    pushed(0x41);
    pushed(0xE9);
    pushed(0x20AC);
    pushed(0x1F600);
    pushed(0xD7FF);
    pushed(0xD800);
    pushed(0xDFFF);
    pushed(0xE000);
    pushed(0x10FFFF);
    pushed(0x110000);
    pushed(-1);

    padded(42, 6, ' ');
    padded(-42, 6, ' ');
    padded(-42, 6, '0');
    padded(-42, 2, ' ');
    padded(-42, 3, ' ');
    padded(-42, 0, ' ');
    padded(-9223372036854775807 - 1, 22, ' ');
    padded(-5, 4, '*');

    print(abs(-0.0), " ", abs(0.0), " ", abs(-1.5), " ", abs(2.5));
    let negz: f32 = -0.0;
    let negf: f32 = -1.5;
    print(abs(negz), " ", abs(negf));

    // As many below half of a large range as above it, within a few standard
    // deviations, where a remainder gave 56 in 100.
    var r = rng { 1 };
    let n: i64 = 3 << 61;
    var below = 0;
    var inside = true;
    for i in 100000 {
        let v = rand_int(r, n);
        if v < n / 2 { below++; }
        if v < 0 || v >= n { inside = false; }
    }
    print(below > 49000 && below < 51000, " ", inside);
    // A small range keeps the sequence it always had.
    var s = rng { 42 };
    var t = rng { 42 };
    var same = true;
    for i in 1000 { if rand_int(s, 1000) != ((rand_u64(t) % 1000) as i64) { same = false; } }
    print(same);
    var w = rng { 5 };
    var top = true;
    for i in 1000 {
        let v = rand_int(w, 9223372036854775807);
        if v < 0 { top = false; }
    }
    print(top);
}
