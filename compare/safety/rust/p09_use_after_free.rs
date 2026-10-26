// Free a buffer, then read from it.
fn main() {
    let v = vec![1i64, 2, 3];
    drop(v);
    println!("{}", v[0]);
}
