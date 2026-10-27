// error: Pair is a root struct, which cannot be written as the type of field O.p
// A root struct owns its fields' stacks: no struct holds one by value.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
struct O { p: Pair }
fn main() {}
