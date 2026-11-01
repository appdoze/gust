// A float's text and reading it back: the shortest digits that read back as
// the same float (Grisu3, with the integers where it cannot prove them), and
// the nearest float to a decimal, ties to the even significand. Each hard
// case's value was checked against Python's float() when this was written.
import std;

struct Rnd { state: u32 }

fn next(r: Rnd&) -> u32 {
    r.state = r.state * 1664525 + 1013904223;
    return r.state;
}

// The full 53 bits, from two draws.
fn draw(r: Rnd&) -> f64 { r.next() * (1.0 / 4294967296.0) + r.next() * (1.0 / 4294967296.0) * (1.0 / 4294967296.0) }

fn round_trips(lo: f64, span: f64, name: u8[:]) {
    var rnd = Rnd { 4242 };
    var out: u8[>..] = [];
    var bad = 0;
    for i in 20000 {
        let v = lo + rnd.draw() * span;
        out.clear();
        format(out, v);
        let back, ok = parse_flt(out);
        if !ok || back != v { bad += 1; }
    }
    print(name, ": ", bad, " of 20000 do not read back");
}

fn show(s: u8[:]) {
    let v, ok = parse_flt(s);
    print(ok, " ", v);
}

fn main() {
    round_trips(0.001, 1000.0, "0.001 to 1000");
    round_trips(1.0, 1.0, "1 to 2");
    round_trips(1000000.0, 1000000000.0, "1e6 to 1e9");
    round_trips(0.0000001, 0.00001, "1e-7 to 1e-5");
    round_trips(1.0e300, 1.0e308, "1e300 to 1e308");
    round_trips(1.0e-300, 1.0e-290, "1e-300 to 1e-290");

    // Digits a double needs in full.
    show("187.89295434454314");
    show("1.8800653687314919");
    show("460199627.27824956");
    show("2.3550527726315364e-07");
    show("0.1");
    show("1e23");
    show("8.41e21");
    // The edges of the range.
    show("1.7976931348623157e308");
    show("1.7976931348623158e308");
    show("1.7976931348623159e308");
    show("1e309");
    show("2.2250738585072014e-308");
    show("2.2250738585072012e-308");
    show("2.225073858507201e-308");
    show("4.9406564584124654e-324");
    show("2.4703282292062328e-324");
    show("2.4703282292062327e-324");
    show("1e-400");
    show("-0.0");
    show("-1e-400");
    // Ties, which go to the even significand, and what is just past them.
    show("9007199254740993");
    show("9007199254740995");
    show("9007199254740993.0000000000000001");
    show("1.00000000000000011102230246251565404236316680908203125");
    show("1.000000000000000111022302462515654042363166809082031250000001");
    show("1.00000000000000033306690738754696212708950042724609375");
    show("123456789012345678901234567890");
    show("0.000000000000000000000000000000000000000000000000000000000000000000000000000001");

    // A tie past 780 digits: a 1 at the very end still rounds up.
    var long: u8[>..] = [];
    long.append("1.00000000000000011102230246251565404236316680908203125");
    let tie, tok = parse_flt(long);
    for i in 900 { long.push('0'); }
    let same, sok = parse_flt(long);
    long.push('1');
    let up, uok = parse_flt(long);
    print(tok, sok, uok, " ", tie, " ", same, " ", up);

    // Forms and refusals as before.
    show("+1.5E+3");
    show(".5");
    show("5.");
    show("");
    show(".");
    show("1e");
    show("abc");
    show("1.5x");

    // Text of doubles and of f32s, all exponents: a hash over many, and some.
    var rnd = Rnd { 99 };
    var h: u64 = 14695981039346656037;
    var hf: u64 = 14695981039346656037;
    var out: u8[>..] = [];
    for i in 60000 {
        var e = (rnd.next() % 600) as! i64 - 300;
        var v = rnd.draw() + 0.5;
        while e > 0 { v *= 10.0; e -= 1; }
        while e < 0 { v /= 10.0; e += 1; }
        out.clear();
        format(out, v);
        for c in out { h = (h ^ c) * 1099511628211; }
        let f = v as f32;
        out.clear();
        format(out, f);
        for c in out { hf = (hf ^ c) * 1099511628211; }
        if i % 12000 == 0 { print(v, " ", f); }
    }
    print(h, " ", hf);
}
