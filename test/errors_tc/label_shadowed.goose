// error: the label outer is already that of a loop or block around this one
// One label per construct among those around a jump, so that it names one.
fn main() {
    outer: for x in 3 {
        outer: for y in 3 {
            if x == y { break :outer; }
        }
    }
}
