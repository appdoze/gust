// error: a label names a loop or a block
// A label goes on a loop or a block, nothing else.
fn main() {
    var x = 0;
    done: for i in 3 { x += i; }
    print(x);
    marked: if x > 2 { print(x); }
}
