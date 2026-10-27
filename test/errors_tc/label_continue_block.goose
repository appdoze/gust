// error: continue :found names a block, which has no next iteration
// A labeled continue names a loop: a block runs once.
fn main() {
    for x in 3 {
        found: block {
            if x == 1 { continue :found; }
        }
    }
}
