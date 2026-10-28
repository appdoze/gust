// error: field p of Ctx holds a reference to a Pair, a root struct, which has no default
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
var g = Pair { .. };
struct Ctx { p: Pair& = g }
fn main() {}
