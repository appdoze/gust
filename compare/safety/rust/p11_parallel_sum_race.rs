// Threads of one scope all add to one total, which they would race on.
fn main() {
    let xs: Vec<i64> = (0..1000).collect();
    let mut total = 0i64;
    std::thread::scope(|s| {
        for chunk in xs.chunks(250) {
            s.spawn(|| {
                for x in chunk {
                    total += x;
                }
            });
        }
    });
    println!("{}", total);
}
