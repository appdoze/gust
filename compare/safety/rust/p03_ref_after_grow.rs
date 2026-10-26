// Keep a reference to element 0, grow the array (which may reallocate), then write through it.
fn main() {
    let mut v = vec![1i64];
    let r = &mut v[0];
    for i in 0..1000 {
        v.push(i);
    }
    *r = 99;
    println!("{}", v[0]);
}
