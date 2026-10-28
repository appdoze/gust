// error: storing a reference rooted at y, which does not outlive the destination (§9.2)
// Nor does rebinding an outer Ctx to an owner of an inner scope pass.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
struct Ctx { p: Pair& }
fn main() {
    var x = Pair { .. };
    var c = Ctx { p: x };
    {
        var y = Pair { .. };
        c.p .= y;
    }
    print(c.p.a.len);
}
