// Recursion that never ends: exhausts the native stack.
import os;
recursive fn f(n: i64) -> i64 {
    if n < 0 { return 0; }
    return f(n + 1) + 1;
}
fn main() {
    print(f(args().len));
}
