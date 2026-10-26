// error: c_callback(forget): it may rebind current
// A test of an optional the caller made before calling C must still hold
// when C returns, so a callback may not rebind one outside it (§7.11).
extern fn run_cb(f: u64);
struct Node { v: i64 }
var nodes: Node[>..] = [];
var current: Node? = null;
export fn forget() { current .= null; }
fn main() {
    nodes.push(Node { v: 1 });
    current .= nodes[0];
    run_cb(c_callback(forget));
}
