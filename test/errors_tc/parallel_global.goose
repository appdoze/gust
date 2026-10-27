// error: a parallel for's body writes the global hits, which every iteration shares
var hits = 0;
fn main() {
    parallel for i in 10 { hits = hits + 1; }
}
