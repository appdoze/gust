// error: field p of Ctx is not given: it holds a reference to a Pair, a root struct, which has no default
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
struct Ctx { p: Pair&, k: i64 = 0 }
fn main() {
    let c = Ctx { k: 1, .. };
    print(c.k);
}
