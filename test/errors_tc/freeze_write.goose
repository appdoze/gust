// error: cannot assign through this path (const, or a read-only instantiation, §9.5)
// What freeze returns is read-only: every thread may be reading it.
fn main() {
    var a: i64[>..] = [1, 2];
    let f = freeze(a);
    f[0] = 5;
}
