// error: cannot move a: the value assigned to a is still under construction, and the shrink would free the storage it is built in (§1.3)
// a = a.move() would build a's new value where the move frees it.
fn main() {
    var a: i64[>..] = [1];
    a = a.move();
    print(a);
}
