// error: break with a value exits loop/block only
// A while has no value for a labeled break to give it either.
fn main() {
    outer: while true {
        loop { break :outer 5; }
    }
}
