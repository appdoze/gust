// error: a parallel for's body writes total, which another iteration may also reach: it may write the enclosing code's arrays only at its own index, as total[i]
// A variable around the loop is every iteration's.
fn main() {
    var total = 0;
    parallel for i in 10 { total += i; }
    print(total);
}
