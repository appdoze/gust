// Keep a reference to element 0, grow the array, then write through it.
fn main() {
    var v: i64[>..] = [1];
    let r .= v[0];
    for i in 1000 { v.push(i); }
    r = 99;
    print(v[0]);
}
