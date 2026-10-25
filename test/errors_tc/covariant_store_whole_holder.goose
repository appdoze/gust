// error: storing a reference rooted at l, which does not outlive the destination
// Gust regression for the shape of rust-lang/rust#164250: a short-lived
// reference must not reach a longer-lived holder through this channel.
// Overwriting the whole holder with a literal that holds a shorter-lived reference.
struct Q { r: i64? }
fn main() {
    var q = Q { r: null };
    {
        var l = 1;
        q = Q { r: l };
    }
    if q.r { print(q.r); }
}
