// error: storing a reference rooted at l, which does not outlive the destination
// Gust regression for the shape of rust-lang/rust#164250: a short-lived
// reference must not reach a longer-lived holder through this channel.
// A variable-mode enum payload in an outer pool given a shorter-lived reference.
enum E { Ref { r: i64? }, None }
fn main() {
    var pool: E..[>..] = [];
    pool.push(E.Ref { r: null });
    {
        var l = 1;
        for e in pool {
            match e { Ref &x => { x.r .= l; }, _ => {} }
        }
    }
    for e in pool { match e { Ref &x => { if x.r { print(x.r); } }, _ => {} } }
}
