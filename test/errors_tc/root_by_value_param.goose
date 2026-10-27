// error: p is a Pair, a root struct: f takes it only by a parameter declared Pair& (or const Pair&)
// A root struct is passed by reference only: its fields are stored apart,
// and a copy of it would be none of them.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
fn f(p: Pair) {}
fn main() {
    var p = Pair { .. };
    f(p);
}
