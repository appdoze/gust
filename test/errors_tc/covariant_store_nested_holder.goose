// error: storing a reference rooted at l, which does not outlive the destination
// Gust regression for the shape of rust-lang/rust#164250: a short-lived
// reference must not reach a longer-lived holder through this channel.
// A holder inside a holder: the store goes two levels down.
struct In { r: i64? }
struct Out { a: In, n: i64 }
fn main() {
    var o = Out { a: In { r: null }, n: 0 };
    {
        var l = 1;
        o.a.r .= l;
    }
    let h = o.a.r;
    if h { print(h); }
}
