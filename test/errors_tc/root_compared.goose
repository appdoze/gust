// error: p is a Pair, a root struct: name one of its fields, or pass it to a Pair& parameter
// A root struct is no value to compare.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
fn main() {
    var p = Pair { .. };
    var q = Pair { .. };
    print(p.n == q.n);
    if p == q { print(1); }
}
