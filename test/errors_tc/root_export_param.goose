// error: a root struct does not cross into C: parameter p of f is a Pair&
// Nor does C.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
export fn f(p: Pair&) {}
fn main() {}
