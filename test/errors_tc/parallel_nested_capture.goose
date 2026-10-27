// error: a parallel for's body calls bump, which writes a global or a variable around the loop
// A nested function's free variable is the enclosing code's, every iteration's.
fn main() {
    var count = 0;
    fn bump() { count += 1; }
    parallel for i in 10 { bump(); }
    print(count);
}
