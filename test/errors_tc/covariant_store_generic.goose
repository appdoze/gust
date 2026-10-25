// error: storing a reference rooted at x, which does not outlive the destination
// Gust regression for the shape of rust-lang/rust#164250: a short-lived
// reference must not reach a longer-lived holder through this channel.
// A generic setter: the field's type comes from the instantiation.
struct Q<T> { r: T? }
fn set<T>(q: Q<T>&, x: T&) { q.r .= x; }
fn main() {
    var q = Q<i64> { r: null };
    {
        var l = 1;
        set(q, l);
    }
    if q.r { print(q.r); }
}
