// error: returning a reference rooted in x, which dies with this function (§9.2)
// A Ctx holding a local owner does not outlive it: each field stored apart
// is a reference the store rule checks.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
struct Ctx { p: Pair&, k: i64 = 0 }
fn make() -> Ctx {
    var x = Pair { .. };
    return Ctx { p: x };
}
fn main() { let c: Ctx = make(); print(c.k); }
