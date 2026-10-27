// error: cannot pop p.a while r (bound at
// As through the owner, through a parameter: the field it shrinks is the one
// the reference points into.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
fn f(p: Pair&) {
    p.a.push(1);
    let r .= p.a[0];
    p.a.pop();
    print(r);
}
fn main() {
    var p = Pair { .. };
    f(p);
}
