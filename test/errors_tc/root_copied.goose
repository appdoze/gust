// error: p is a Pair, a root struct, which copy does not take: pass it one of its fields
// No builtin takes a root struct: not copy, print, to_bytes or thread_spawn.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
fn main() {
    var p = Pair { .. };
    var q = copy(p);
}
