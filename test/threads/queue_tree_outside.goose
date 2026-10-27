// Gust: what a queue carries is checked as it is sent (§11.2). Both ends of
// a self-relative link lie in one root, t here, but qput sends t.a alone,
// and its first node's link leads into t.b, which stays behind: the put
// aborts rather than hand the receiver a link to nowhere.
struct Link { v: i64, next: Link&<u32>? }
struct Two { a: Link[2], b: Link[2] }

fn main() {
    var t = Two { a: [Link { v: 1, next: null }, Link { v: 2, next: null }],
                  b: [Link { v: 3, next: null }, Link { v: 4, next: null }] };
    t.a[0].next .= &t.b[1];
    print("sending");
    qput(t.a);
    print("not reached");
}
