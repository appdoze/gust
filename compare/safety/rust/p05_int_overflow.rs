// Signed overflow at run time, built with the same optimised flags as the benchmarks.
fn main() {
    let n = std::env::args().count() as i64;
    let x: i64 = i64::MAX + n;
    println!("{}", x);
}
