// error: Pair is a root struct, which cannot be written as the type of parameter g of apply
// A function value takes no root struct: a function type naming one could
// not say where its fields go.
root struct Pair { a: i64[>..<] = [], b: i64[>..<] = [], n: i64 = 0 }
fn apply(g: fn(Pair&)) {}
fn main() {}
