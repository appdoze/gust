// error: c_callback's argument names an export fn
// Only an export fn has a C wrapper whose address C can call (§7.11).
fn step(x: i64) -> i64 { x + 1 }
fn main() {
    print(c_callback(step));
}
