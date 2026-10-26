// Integer division by a zero only known at run time.
fn main() {
    let n = std::env::args().count() as i64;
    println!("{}", 10 / (n - 1));
}
