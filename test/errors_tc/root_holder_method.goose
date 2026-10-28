// error: Ctx.p holds a Pair, a root struct, which f takes only from a name declared Ctx
// And so does a method call on it.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
struct Ctx { p: Pair& }
fn f(p: Pair&) { p.n += 1; }
fn make(p: Pair&) -> Ctx { return Ctx { p: p }; }
fn main() {
    var x = Pair { .. };
    make(x).p.f();
}
