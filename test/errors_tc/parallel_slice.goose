// error: a parallel for's body writes s, which another iteration may also reach: it may write the enclosing code's arrays only at its own index, as s[i]
// A slice may be another name for what an iteration reads: an owned array is not.
fn fill(s: i64[:]) {
    parallel for i in s.len { s[i] = i; }
}
fn main() {
    var a: i64[4] = [0, 0, 0, 0];
    fill(a);
}
