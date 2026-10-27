// error: a parallel for's body calls bump, which writes a global or a variable around the loop
var count = 0;
fn bump(x: i64) -> i64 { count = count + 1; return x; }
fn main() {
    var out: i64[10] = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    parallel for i in 10 { out[i] = bump(i); }
}
