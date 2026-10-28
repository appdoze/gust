// error: Pair is a root struct, which cannot be written as the type of field O.ps
// An alias is no way around the rules: its uses are checked as the type it
// names, here an array of references to a root struct.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
type PR = Pair&;
struct O { ps: PR[2] }
fn main() {}
