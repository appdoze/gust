// error: a parallel for's body hands for writing src, which another iteration may also reach: it may write the enclosing code's arrays only at its own index, as src[i]
// A callee that writes what it is handed writes it for every iteration.
fn zero(xs: i64[:]) { xs[0] = 0; }
fn main() {
    var src: i64[3] = [1, 2, 3];
    var out: i64[3] = [0, 0, 0];
    parallel for i in 3 {
        zero(src);
        out[i] = i;
    }
}
