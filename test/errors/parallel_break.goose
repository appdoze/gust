// error: no break leaves a parallel for: its iterations run apart, on other threads (continue ends one)
fn main() {
    parallel for i in 10 {
        if i == 3 { break; }
    }
}
