// error: storing a reference rooted at val, which does not outlive the destination
// Gust regression for the shape of rust-lang/rust#164250: a short-lived
// reference must not reach a longer-lived holder through this channel.
// A dictionary in an outer scope keeps a reference to an inner local.
import dictionary;
fn main() {
    var d = dictionary<i64, i64?> {};
    {
        var l = 1;
        d.insert(7, l);
    }
    let v = d.get(7);
    if v { print(v); }
}
