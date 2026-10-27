// Gust: each iteration writes the next element, which the next iteration
// reads and writes: a race on every element but the first.
import std;
fn main() {
    var out: i64[>..] = [];
    out.push_n(1, 1000);
    parallel for i in 999 { out[i + 1] = out[i] * 2 % 1000003; }
    print(out[999]);
}
