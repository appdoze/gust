// error: field a of root struct Pair is stored apart from the struct: through Ctx.p, reach it from a name declared Ctx
// Nor a for loop's variable.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
struct Ctx { p: Pair& }
fn main() {
    var x = Pair { .. };
    var cs: Ctx[>..] = [];
    cs.push(Ctx { p: x });
    for c in cs { print(c.p.a.len); }
}
