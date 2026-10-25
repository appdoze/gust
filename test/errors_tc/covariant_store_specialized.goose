// error: storing a reference rooted at x, which does not outlive the destination
// Gust regression for the shape of rust-lang/rust#164250: a short-lived
// reference must not reach a longer-lived holder through this channel.
// One setter called first with a valid root, then with a shorter one: the
// second call must not reuse the first call's checked specialization.
struct Q { r: i64? }
fn keep(dst: Q&, x: i64&) { dst.r .= x; }
fn main() {
    var g = 5;
    var q = Q { r: null };
    keep(q, g);
    {
        var l = 1;
        keep(q, l);
    }
    if q.r { print(q.r); }
}
