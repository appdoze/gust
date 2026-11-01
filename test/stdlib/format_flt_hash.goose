// format_flt from the float's exact value, at any size, and hashes whose low
// bits, which pick a dictionary's group, differ however the keys differ.
import std;
import dictionary;

fn show(v: f64, d: i64) {
    var out: u8[>..] = [];
    format_flt(out, v, d);
    print(out);
}

// How many of 4096 low-bit values the keys reach; 2000 keys at random reach
// about 1600.
fn spread<F>(name: u8[:]) {
    var seen: bool[4096] = [false; 4096];
    var n = 0;
    for i in 2000 {
        let h = F(i);
        let b = (h & 4095) as! i64;
        if !seen[b] { seen[b] = true; n += 1; }
    }
    print(name, ": ", n > 1200);
}

fn main() {
    // Below or above a tie by far less than the multiplication's rounding.
    show(1.115, 2);
    show(2.675, 2);
    show(1.005, 2);
    show(0.285, 2);
    show(1.45, 1);
    show(8.345, 2);
    // Exact ties go up.
    show(0.125, 2);
    show(0.375, 2);
    show(2.5, 0);
    show(3.5, 0);
    show(-2.5, 0);
    // Past what an i64 holds once scaled, or as a scale.
    show(1.0e19, 2);
    show(1.8446744073709552e19, 0);
    show(1.0e22, 1);
    show(123.456, 20);
    show(0.1, 25);
    // Carries, through the point and past the first digit.
    show(9.995, 2);
    show(9.999, 2);
    show(99.5, 0);
    show(0.9999, 3);
    // Small, where the rest of the digits are in integers.
    show(0.001, 3);
    show(0.0005, 3);
    show(0.00049, 3);
    show(1.0e-300, 2);
    show(5.0e-324, 2);
    show(-0.001, 2);
    show(0.0, 3);
    show(-0.0, 1);
    // No digits, and more than a double has.
    show(0.5, 0);
    show(0.49, 0);
    show(1.5, -1);
    var long: u8[>..] = [];
    format_flt(long, 5.0e-324, 1200);
    print(long.len, " ", long[1070..1080], " ", long[long.len - 3..]);
    var big: u8[>..] = [];
    format_flt(big, 1.7976931348623157e308, 3);
    print(big.len, " ", big[..20], " ", big[big.len - 6..]);

    // Keys that differ only in high bits, or by small amounts as floats.
    spread("i64 << 44") { i => (i << 44).hash() };
    spread("i64 << 52") { i => (i << 52).hash() };
    spread("i64 << 32") { i => (i << 32).hash() };
    spread("i64 plain") { i => i.hash() };
    spread("f64 whole") { i => (i + 0.0).hash() };
    spread("f64 1e-9 steps") { i => (i * 0.000000001).hash() };
    spread("f64 2^48 steps") { i => (i * 281474976710656.0).hash() };
    spread("f32 1e-6 steps") { i => ((i * 0.000001) as f32).hash() };
    // The two zeros compare equal, so they must find each other.
    var d = dictionary<f64, i64> {};
    d.insert(0.0, 1);
    let z = -0.0;
    print(d.contains(z), " ", 0.0.hash() == z.hash());
    // Such keys in a dictionary, in time that would show if they all collided.
    var e = dictionary<i64, i64> {};
    for i in 100000 { e.insert(i << 44, i); }
    var found = 0;
    for i in 100000 { if e.contains(i << 44) { found += 1; } }
    print(found);
}
