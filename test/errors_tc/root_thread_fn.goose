// error: a thread_fn takes no root struct: parameter p of w is a Pair&
// A worker takes no root struct: it would have to copy each field.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
thread_fn w(p: Pair&) {}
fn main() {}
