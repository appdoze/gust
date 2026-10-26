// A return, break or continue where a value is wanted -- an array literal's
// element, a let's initializer, alone or as an if's branch -- leaves there:
// nothing after it runs (found by bench/compiler_fuzz.py: `[1, return 5]`
// and `let r: i64& = return x` failed in code generation). Fixed,
// resizable and struct-element literals, and value and reference lets.
struct P { a: i64, b: i64 }

fn leave_direct(c: bool) -> i64 {
    if c {
        let xs: i64[>..] = [1, return 5];
        return xs[0];
    }
    return 0;
}

fn walk(n: i64) -> i64 {
    var total = 0;
    for k in n {
        let a: i64[4] = [k, if k == 2 { continue } else { 1 }, 3, 4];
        let ps: P[>..] = [P { a: k, b: 1 }, if k == 4 { break } else { P { a: 0, b: 0 } }];
        total += a[0] + ps.len;
        let more: i64[>..] = [k, if k == 3 { return total } else { 0 }];
        total += more[0];
    }
    return total;
}

fn leave_let(c: bool) -> i64 {
    var xs: i64[>..] = [1, 2, 3];
    if c {
        let r: i64& = return xs[2];
        return r;
    }
    let v: i64 = return 9;
    return v;
}

fn walk_lets(n: i64) -> i64 {
    var t = 0;
    for k in n {
        let a: i64 = if k == 1 { continue } else { k };
        let b: i64 = if k == 4 { break } else { 1 };
        t += a + b;
    }
    return t;
}

fn main() {
    print(leave_direct(true), " ", leave_direct(false));
    print(walk(10), " ", walk(3));
    print(leave_let(true), " ", leave_let(false), " ", walk_lets(10));
}
