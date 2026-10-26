// Use a value that may be absent without checking it.
struct Node { v: i64, next: Node? }
fn main() {
    let n = Node { v: 1, next: null };
    print(n.next.v);
}
