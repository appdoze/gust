// error: p is a Pair, a root struct: f takes it only by a parameter declared Pair& (or const Pair&)
// A root struct goes to a parameter declared to take it, which takes its fields
// too: not to an untyped one.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
fn f(p) {}
fn main() {
    var p = Pair { .. };
    f(p);
}
