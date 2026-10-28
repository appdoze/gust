// error: field a of root struct Pair is stored apart from the struct: through Ctx.p, reach it from a name declared Ctx
// A binding of a call's result, untyped, is not a name whose type is
// written: declare it `let c: Ctx = make(x);`.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
struct Ctx { p: Pair& }
fn make(p: Pair&) -> Ctx { return Ctx { p: p }; }
fn main() {
    var x = Pair { .. };
    let c = make(x);
    print(c.p.a.len);
}
