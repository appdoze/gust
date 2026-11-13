// error: a parallel for's body calls show, which prints, in whichever order the iterations run
fn show(x: i64) -> i64 { print(x); return x; }
fn main() {
    var out: i64[4] = [0, 0, 0, 0];
    parallel for i in 4 { out[i] = show(i); }
}
