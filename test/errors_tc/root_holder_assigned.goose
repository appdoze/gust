// error: c.p refers to a Pair, a root struct, which is not assigned whole: rebind it, c.p .= y
// `=` writes through a reference field; `.=` rebinds it, and every field
// stored apart with it.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
struct Ctx { p: Pair& }
fn main() {
    var x = Pair { .. };
    var y = Pair { .. };
    var c = Ctx { p: x };
    c.p = y;
}
