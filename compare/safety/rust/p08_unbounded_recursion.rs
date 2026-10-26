// Recursion that never ends: exhausts the native stack. black_box keeps the optimiser from
// turning the recursion into a loop, which it otherwise does.
fn f(n: u64) -> u64 {
    let a = std::hint::black_box([n; 16]);
    f(n + 1) + a[(n % 16) as usize]
}
fn main() {
    let n = std::env::args().count() as u64;
    println!("{}", f(n));
}
