// error: field b of root struct Pair is not given: add it, or `..` for its default
// A field stored apart is constructed at the owner's declaration, as any
// resizable variable is (§4.2).
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
fn main() {
    var p = Pair { a: [], n: 1 };
}
