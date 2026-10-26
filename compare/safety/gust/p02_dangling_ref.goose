// Return a reference to a local that dies when the function returns.
fn get() -> i64& {
    var x = 5;
    return x;
}
fn main() {
    print(get());
}
