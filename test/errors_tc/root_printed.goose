// error: p is a Pair, a root struct, which print does not take: pass it one of its fields
// A root struct is printed field by field.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
fn main() {
    var p = Pair { .. };
    print(p);
}
