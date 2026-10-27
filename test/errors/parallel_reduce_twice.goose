// error: a parallel for's body reduces total once
fn main() {
    var total = 0;
    parallel for i in 10 {
        reduce total += i;
        reduce total += 1;
    }
    print(total);
}
