// Gust: every iteration of a parallel for adds to one total, which they
// would race on. The checker refuses it: an iteration writes only its own
// element of an array around the loop.
import std;
fn main() {
    var xs: i64[>..] = [];
    for i in 1000 { xs.push(i); }
    var total = 0;
    parallel for i in xs.len { total += xs[i]; }
    print(total);
}
