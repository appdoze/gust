// error: Pair is a root struct, which cannot be written as the type of field O.p
// Nor a reference to one, which would have to carry each field's.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
struct O { p: Pair& }
fn main() {}
