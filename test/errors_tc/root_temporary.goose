// error: Pair is a root struct, which cannot be written here
// A root struct owner is a variable: its literal constructs nothing else.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
fn f(p: Pair&) {}
fn main() {
    f(Pair { .. });
}
