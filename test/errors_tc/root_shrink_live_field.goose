// error: cannot pop p.a while r (bound at
// A root struct's fields are regions of their own: shrinking the one a
// reference points into is still refused (§5.2).
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
fn main() {
    var p = Pair { .. };
    p.a.push(1);
    let r .= p.a[0];
    p.a.pop();
    print(r);
}
