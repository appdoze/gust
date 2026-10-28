// error: cannot .push through a non-writable value
// A field holding a const Pair& carries const references to its fields.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
struct View { p: const Pair& }
fn f(v: View) { v.p.a.push(1); }
fn main() {
    var x = Pair { .. };
    f(View { p: x });
}
