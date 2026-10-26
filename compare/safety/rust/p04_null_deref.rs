// Use a value that may be absent without checking it.
struct Node {
    v: i64,
    next: Option<Box<Node>>,
}
fn main() {
    let n = Node { v: 1, next: None };
    println!("{}", n.next.v);
}
