// error: cannot .push through a non-writable value
// A const root struct parameter's fields are const too.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
fn f(p: const Pair&) { p.a.push(1); }
fn main() {
    var p = Pair { .. };
    f(p);
}
