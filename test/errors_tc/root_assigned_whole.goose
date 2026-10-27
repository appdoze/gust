// error: p is a Pair, a root struct, which is not assigned whole: assign its fields
// A root struct is not assigned whole.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
fn main() {
    var p = Pair { .. };
    var q = Pair { .. };
    p = q;
}
