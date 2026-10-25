// error: storing a reference rooted at l, which does not outlive the destination
// Gust regression for the shape of rust-lang/rust#164250: a short-lived
// reference must not reach a longer-lived holder through this channel.
// copy() of a holder keeps its references; the copy goes to an outer variable.
struct Q { r: i64& }
fn main() {
    var g = 5;
    var outer = Q { r: g };
    {
        var l = 1;
        var inner = Q { r: l };
        outer = copy(inner);
    }
    print(outer.r);
}
