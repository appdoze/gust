// error: parameter p of f takes a Pair, a root struct, which is passed by name
// A Pair& bound from something other than a root struct value is no root
// struct value itself.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
struct Ctx { p: Pair& }
fn f(p: Pair&) { p.n += 1; }
fn make(p: Pair&) -> Ctx { return Ctx { p: p }; }
fn main() {
    var x = Pair { .. };
    let q = make(x).p;
    f(q);
}
