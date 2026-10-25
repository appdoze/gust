// error: storing a reference rooted at l, which does not outlive the destination
// Gust regression for the shape of rust-lang/rust#164250: a short-lived
// reference must not reach a longer-lived holder through this channel.
// A trailing block writes an enclosing holder from a shorter-lived scope.
struct Q { r: i64? }
fn each<F>(n: i64) { for i in n { F(i); } }
fn main() {
    var q = Q { r: null };
    each(3) {
        var l = it;
        q.r .= l;
    };
    if q.r { print(q.r); }
}
