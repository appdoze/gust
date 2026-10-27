// Gust: a.move(), the assignment that leaves its source empty.
struct Pt { x: i64, y: i64 }

fn total(a: i64[>..]) -> i64 { var s = 0; for v in a { s += v; } s }
fn taken(a: i64[>..]&) -> i64[>..] { return a.move(); }

fn main() {
    var a: i64[>..] = [1, 2, 3];
    let b = a.move();
    print(a.len, " ", b);                         // 0 [1, 2, 3]
    a.push(9);
    print(a);                                     // [9]

    var x: i64[>..] = [7];
    a.append([10, 11]);
    x = a.move();
    print(a.len, " ", x);                         // 0 [9, 10, 11]

    var s: u8[..8] = "abc";
    let t = s.move();
    print(s.len, " ", t);                         // 0 abc

    var names: u8[..16][>..] = [];
    names.push("ab");
    names.push("cde");
    let moved = names.move();
    print(names.len, " ", moved);                 // 0 ["ab", "cde"]

    var pts: Pt[>..<] = [Pt { 1, 2 }, Pt { 3, 4 }];
    var others = pts.move();
    others.push(Pt { 5, 6 });
    print(pts.len, " ", others.len, " ", others[2].x);   // 0 3 5

    a.append([4, 5]);
    print(total(a.move()), " ", a.len);           // 9 0
    a.append([6]);
    print(a.move().len, " ", a.len);              // 1 0
    a.append([1, 2]);
    let r = taken(a);
    print(r, " ", a.len);                         // [1, 2] 0
}
