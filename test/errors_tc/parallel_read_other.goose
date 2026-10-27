// error: a parallel for's body reads out other than at out[i], where another iteration writes
// What another iteration writes is no iteration's to read.
import std;
fn main() {
    var out: i64[>..] = [];
    out.push_n(1, 10);
    parallel for i in 10 { out[i] = out[i] + out[0]; }
}
