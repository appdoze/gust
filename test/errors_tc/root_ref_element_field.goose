// error: Pair is a root struct, which cannot be written as the type of field O.ps
// Nor is a reference to one an element, which would need a parallel array
// of each field's.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
struct O { ps: Pair&[2] }
fn main() {}
