// error: a parallel for's body cannot return: its iterations run apart, on other threads (continue ends one)
fn main() {
    parallel for i in 10 {
        if i == 3 { return; }
    }
}
