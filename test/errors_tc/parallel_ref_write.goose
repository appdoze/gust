// error: a parallel for's body writes through r, which may point outside the body
// A reference to another element is another iteration's element.
struct P { x: i64 }
fn main() {
    var ps: P[4] = [P { 1 }, P { 2 }, P { 3 }, P { 4 }];
    parallel for i in 3 {
        let r = &ps[i + 1];
        r.x = 0;
    }
}
