// error: Pair is a root struct, which cannot be written as the type of field O.p
// An alias is no way around the rules: its uses are checked as the type it names.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
type PR = Pair&;
struct O { p: PR }
fn main() {}
