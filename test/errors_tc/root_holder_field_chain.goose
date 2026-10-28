// error: field a of root struct Pair is stored apart from the struct: through Ctx.p, reach it from a name declared Ctx
// Nor a field of a field: the Ctx is bound to a name of its own first.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
struct Ctx { p: Pair& }
struct Outer { c: Ctx, k: i64 = 0 }
fn main() {
    var x = Pair { .. };
    let o = Outer { c: Ctx { p: x } };
    print(o.c.p.a.len);
}
