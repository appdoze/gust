// Two threads write elements the other reads.
fn main() {
    let mut out = vec![1i64; 1000];
    std::thread::scope(|s| {
        s.spawn(|| {
            for i in 0..499 {
                out[i + 1] = out[i] * 2 % 1000003;
            }
        });
        s.spawn(|| {
            for i in 499..999 {
                out[i + 1] = out[i] * 2 % 1000003;
            }
        });
    });
    println!("{}", out[999]);
}
