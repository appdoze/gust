// error: cannot move a while r (bound at
// A reference into a grow-shrink array outlives no shrink of it (§5.2),
// and a move is one.
fn main() {
    var a: i64[>..<] = [1, 2];
    let r .= a[0];
    let b = a.move();
    print(r, b);
}
