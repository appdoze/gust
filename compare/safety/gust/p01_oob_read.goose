// Read past the end of an array, with an index only known at run time.
import os;
fn main() {
    let n = args().len;
    let v: i64[>..] = [1, 2, 3, 4, 5];
    print(v[n + 9]);
}
