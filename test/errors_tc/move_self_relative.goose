// error: .move copies the elements out: copying a value of type N, which contains self-relative references, is not supported
// A move copies the elements out of their array, and a self-relative link
// measured from where an element was would point elsewhere from the copy.
struct N { v: i64, next: N&<u32>? }
fn main() {
    var a: N[>..] = [];
    a.push(N { v: 1 });
    let b = a.move();
    print(b.len);
}
