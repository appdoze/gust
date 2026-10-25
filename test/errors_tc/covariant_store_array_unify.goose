// error: storing a reference rooted at l, which does not outlive the destination
// Gust regression for the shape of rust-lang/rust#164250: a short-lived
// reference must not reach a longer-lived holder through this channel.
// The issue's array-unification variant: one array holding both roots,
// an element of which is copied to an outer holder.
struct Q { r: i64& }
fn main() {
    var g = 5;
    var outer = Q { r: g };
    {
        var l = 1;
        var arr: Q[2] = [Q { r: g }, Q { r: l }];
        outer = arr[1];
    }
    print(outer.r);
}
