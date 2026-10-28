// error: c.p is a Pair, a root struct, which a function does not return
// A Pair reached through a Ctx is not returned either.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
struct Ctx { p: Pair& }
fn f(c: Ctx) -> Pair& { return c.p; }
fn main() {}
