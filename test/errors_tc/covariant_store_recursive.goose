// error: storing a reference rooted at l, which does not outlive the destination
// Gust regression for the shape of rust-lang/rust#164250: a short-lived
// reference must not reach a longer-lived holder through this channel.
// A recursive function stores a local of one activation into a caller's holder.
struct Q { r: i64? }
recursive fn walk(q: Q&, n: i64) {
    var l = n;
    if n > 0 { walk(q, n - 1); }
    q.r .= l;
}
fn main() {
    var q = Q { r: null };
    walk(q, 3);
    if q.r { print(q.r); }
}
