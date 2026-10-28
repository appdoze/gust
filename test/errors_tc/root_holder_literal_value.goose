// error: field p of Ctx holds a reference to a Pair, a root struct, which it is given by name
// A literal's field holding a Pair is given a root struct value, which a
// call's result is not.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
struct Ctx { p: Pair& }
fn make(p: Pair&) -> Ctx { return Ctx { p: p }; }
fn main() {
    var x = Pair { .. };
    let c = Ctx { p: make(x).p };
}
