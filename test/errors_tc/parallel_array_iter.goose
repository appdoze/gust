// error: a parallel for counts, `parallel for i in n` or `in a..b`: index an array with i
fn main() {
    let xs: i64[3] = [1, 2, 3];
    parallel for x in xs { print(x); }
}
