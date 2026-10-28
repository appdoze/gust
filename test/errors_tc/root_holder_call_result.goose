// error: field a of root struct Pair is stored apart from the struct: through Ctx.p, reach it from a name declared Ctx
// The Pair a call's result holds is reached through a name whose type is
// written, which the call's result is not.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
struct Ctx { p: Pair& }
fn make(p: Pair&) -> Ctx { return Ctx { p: p }; }
fn main() {
    var x = Pair { .. };
    make(x).p.a.push(1);
}
