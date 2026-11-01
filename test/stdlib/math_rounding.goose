// Gust: floor, ceil, trunc and round, and float %, are the runtime's own
// (runtime_ext.h, runtime.h gs_fmod), with no libm: each gives libm's result
// to the bit, at both widths, around the edges. The values are read from
// arrays, so that none is known to the optimizer. A zero's sign shows as
// the sign of 1 / it.
import math;
import std;

fn show(x: f64) {
    if x == 0.0 { print(if 1.0 / x < 0.0 { "-0" } else { "+0" }); }
    else { print(x); }
}

fn showf(x: f32) {
    if x == 0.0 { print(if 1.0 / x < 0.0 { "-0" } else { "+0" }); }
    else { print(x); }
}

fn main() {
    let z = 0.0;
    var xs: f64[>..] = [0.0, -0.0, 0.3, -0.3, 0.5, -0.5, 0.7, -0.7, 1.5, -1.5, 2.5, -2.5,
                        0.49999999999999994, -0.49999999999999994, 1.0, -1.0,
                        4503599627370495.5, -4503599627370495.5, 4503599627370496.0,
                        -4503599627370497.0, 9007199254740993.0, 1.0e300, -1.0e300,
                        5.0e-324, -5.0e-324, 2.2250738585072014e-308];
    xs.push(1.0 / z);
    xs.push(-1.0 / z);
    xs.push(z / z);
    for x in xs {
        show(floor(x)); show(ceil(x)); show(trunc(x)); show(round(x));
    }
    var fs: f32[>..] = [0.0, -0.0, 0.3, -0.3, 0.5, -0.5, 2.5, -2.5, 0.49999997, 8388607.5,
                        -8388607.5, 8388608.0, 16777215.0, 3.0e38, 1.0e-45];
    for x in fs {
        showf(floor(x)); showf(ceil(x)); showf(trunc(x)); showf(round(x));
    }
    // Float %: C's fmod, the remainder with x's sign.
    var xa: f64[>..] = [5.0, -5.0, 5.0, 5.5, 1.0e300, 1.0e-310, 3.0, 3.0, 3.0, 0.0, -0.0,
                        7.0, -7.0, 1.0e308, 18446744073709551616.0, -2.0, 1.0e-320, 6.5];
    var xb: f64[>..] = [2.0, 2.0, -2.0, 2.0, 3.0, 3.0e-320, 1.0 / z, 0.0, z / z, 3.0, 3.0,
                        7.0, 7.0, 1.0e-308, 3.0, 2.0, 3.0e-321, 0.25];
    xa.push(1.0 / z);
    xb.push(3.0);
    for a, i in xa { show(a % xb[i]); }
    var af: f32[>..] = [5.5, -5.5, 1.0e38, 1.0e-40, 7.0];
    var bf: f32[>..] = [2.0, 2.0, 3.0, 3.0e-42, -7.0];
    for a, i in af { showf(a % bf[i]); }
    // as! beyond i64 wraps modulo 2^64 through the same remainder.
    var big: f64[>..] = [18446744073709551616.0 * 3.0 + 4096.0, -9223372036854775808.0 * 3.0,
                         1.0e19, -1.0e19];
    for b in big { print(b as! i64); }
}
