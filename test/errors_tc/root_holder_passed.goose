// error: Ctx.p holds a Pair, a root struct, which f takes only from a name declared Ctx
// Passing the Pair an element holds to a Pair& parameter needs the
// element bound to a name declared Ctx first.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
struct Ctx { p: Pair& }
fn f(p: Pair&) { p.n += 1; }
fn main() {
    var x = Pair { .. };
    var cs: Ctx[>..] = [];
    cs.push(Ctx { p: x });
    f(cs[0].p);
}
