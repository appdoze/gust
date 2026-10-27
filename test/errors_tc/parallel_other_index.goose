// error: a parallel for's body writes out, which another iteration may also reach: it may write the enclosing code's arrays only at its own index, as out[i]
// The next element is the next iteration's.
import std;
fn main() {
    var out: i64[>..] = [];
    out.push_n(0, 11);
    parallel for i in 10 { out[i + 1] = i; }
}
