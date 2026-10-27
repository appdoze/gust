// error: .index_of needs a reference rooted at the array itself (§3.3); this one is rooted at p.a
// index_of names one field of a root struct: two fields of one type are two
// arrays.
root struct Pair { a: i64[>..] = [], b: i64[>..] = [] }
fn main() {
    var p = Pair { .. };
    let r .= p.a.push(5);
    p.b.push(6);
    print(p.b.index_of(r));
}
