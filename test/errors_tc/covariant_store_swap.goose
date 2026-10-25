// error: storing a reference rooted at b, which does not outlive the destination
// Gust regression for the shape of rust-lang/rust#164250: a short-lived
// reference must not reach a longer-lived holder through this channel.
// The issue's swap variant: exchange an outer holder with an inner one.
import std;
struct Q { r: i64& }
fn main() {
    var g = 5;
    var outer = Q { r: g };
    {
        var l = 1;
        var inner = Q { r: l };
        swap(outer, inner);
    }
    print(outer.r);
}
