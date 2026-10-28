// error: an if binding q chooses among values of different root structs
// An if binding of a root struct value chooses among owners of one root
// struct: its fields are the chosen one's.
root struct A { xs: i64[>..] = [] }
root struct B { ys: i64[>..] = [] }
fn main() {
    var a = A { .. };
    var b = B { .. };
    let q = if a.xs.len == 0 { a } else { b };
}
