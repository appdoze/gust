// Read past the end of an array, with an index only known at run time.
fn main() {
    let n = std::env::args().count();
    let v = vec![1i64, 2, 3, 4, 5];
    println!("{}", v[n + 9]);
}
