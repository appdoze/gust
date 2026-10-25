// error: storing a reference rooted at l, which does not outlive the destination
// Gust regression for the shape of rust-lang/rust#164250: a short-lived
// reference must not reach a longer-lived holder through this channel.
// A nested function stores its own local into the enclosing function's holder.
struct Q { r: i64? }
fn main() {
    var q = Q { r: null };
    fn fill() {
        var l = 1;
        q.r .= l;
    }
    fill();
    if q.r { print(q.r); }
}
