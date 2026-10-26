// error: a function value can be called or passed on, but not stand as a statement
// A function value is compile-time only (§7.6): named alone as a statement
// it has no value to compute (found by bench/compiler_fuzz.py: code
// generation crashed on it).
fn apply<F>(x: i64) -> i64 {
    F;
    return x;
}
fn main() { print(apply(2) { k => k }); }
