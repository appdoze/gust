// error: Pair is a root struct, which cannot be written as the type of parameter p of f
// An optional reference to a root struct is not one of the forms it takes.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
fn f(p: Pair?) {}
fn main() {}
