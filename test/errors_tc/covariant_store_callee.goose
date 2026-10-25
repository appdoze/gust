// error: storing a reference rooted at x, which does not outlive the destination
// Gust regression for the shape of rust-lang/rust#164250: a short-lived
// reference must not reach a longer-lived holder through this channel.
// The store happens in a callee that cannot see the caller's scopes.
struct Q { r: i64? }
fn set(q: Q&, x: i64&) { q.r .= x; }
fn main() {
    var q = Q { r: null };
    {
        var l = 1;
        set(q, l);
    }
    if q.r { print(q.r); }
}
