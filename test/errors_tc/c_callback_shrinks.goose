// error: c_callback(drain): it may shrink queue
// A callback runs while the code that called C waits in the call, with its
// references still in use, so it may grow what lies outside it but not
// shrink it (§7.11).
extern fn run_cb(f: u64);
var queue: i64[>..<] = [1, 2, 3];
export fn drain() { queue.clear(); }
fn main() {
    run_cb(c_callback(drain));
}
