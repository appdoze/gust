// Return a reference to a local that dies when the function returns.
fn get<'a>() -> &'a i64 {
    let x = 5i64;
    &x
}
fn main() {
    println!("{}", get());
}
