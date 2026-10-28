// error: Pair is a root struct, which cannot be written as the type of field Q.p
// A root struct holds no reference to one: only an ordinary struct's field
// carries each field's.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
root struct Q { p: Pair&, items: i64[>..] = [] }
fn main() {}
