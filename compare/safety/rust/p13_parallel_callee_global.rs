// A function the threads call counts its calls in a mutable static.
static mut CALLS: i64 = 0;
fn scaled(x: i64) -> i64 {
    CALLS += 1;
    x * 3
}
fn main() {
    let mut out = vec![0i64; 100];
    std::thread::scope(|s| {
        for (k, chunk) in out.chunks_mut(25).enumerate() {
            s.spawn(move || {
                for (j, o) in chunk.iter_mut().enumerate() {
                    *o = scaled((k * 25 + j) as i64);
                }
            });
        }
    });
    println!("{}", out[99]);
}
