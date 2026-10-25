// error: storing a reference rooted at l, which does not outlive the destination
// Gust regression for the shape of rust-lang/rust#164250: a short-lived
// reference must not reach a longer-lived holder through this channel.
// A merged value: the outer holder gets whichever reference a branch picks.
struct Q { r: i64? }
fn main() {
    var g = 5;
    var q = Q { r: null };
    for i in 2 {
        var l = i;
        q.r .= if i == 0 { &g } else { &l };
    }
    if q.r { print(q.r); }
}
