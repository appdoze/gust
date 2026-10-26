// Two threads increment the same counter without synchronisation.
fn main() {
    let mut count = 0i64;
    let h = std::thread::spawn(|| {
        for _ in 0..1000000 {
            count += 1;
        }
    });
    for _ in 0..1000000 {
        count += 1;
    }
    h.join().unwrap();
    println!("{}", count);
}
