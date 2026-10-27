// error: p is a Pair, a root struct: construct it where it is declared, var p = Pair { .. }
// An owner is constructed by its literal where it is declared.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
fn make() -> i64 { return 1; }
fn main() {
    var p: Pair = make();
}
