// error: cannot move a while v is still used: it may hold a reference or slice into it (§5.1)
// A move empties its source as clear does, so it is checked as clear is:
// nothing may still view what it frees.
fn main() {
    var a: i64[>..] = [1, 2];
    let v = a[0..1];
    let b = a.move();
    print(v, b);
}
