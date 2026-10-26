// Signed overflow at run time, built with the same optimised flags as the benchmarks.
import os;
fn main() {
    let n = args().len;
    let x = 9223372036854775807 + n;
    print(x);
}
