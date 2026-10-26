// Integer division by a zero only known at run time.
import os;
fn main() {
    let n = args().len;
    print(10 / (n - 1));
}
