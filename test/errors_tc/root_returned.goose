// error: p is a Pair, a root struct, which a function does not return
// A root struct value is not returned, by reference or otherwise.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
fn f(p: Pair&) -> Pair& { return p; }
fn main() {}
