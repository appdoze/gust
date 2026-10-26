// Keep a reference to the last element of a stack, pop it, then write through it.
fn main() {
    var v: i64[>..<] = [1, 2];
    let r .= v[1];
    v.pop();
    r = 99;
    print(v);
}
