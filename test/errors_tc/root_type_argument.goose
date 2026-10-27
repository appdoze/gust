// error: Pair is a root struct, which cannot be written here
// A root struct is no type argument: Box<Pair> would hold one by value.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
struct Box<T> { v: T }
fn main() {
    var b = Box<Pair> { .. };
}
