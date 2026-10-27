// Gust: a labeled break or continue from a nested loop leaves an iteration
// of the loop it names part-run, so a counted push loop it can cut short
// states no length, and a while it can leave says nothing of its condition
// after it. One that names a loop further out never reaches this loop's end.

fn skipped_push(xs: i64[:]) -> i64 {
    var a: i64[>..] = [];
    outer: for i in xs.len {
        for k in 2 { if xs[i] == 3 && k == 1 { continue :outer; } }
        a.push(xs[i]);
    }
    return a[xs.len - 2];                    // bce:keep (a continue :outer skipped a push)
}

fn whole_push(xs: i64[:]) -> i64 {
    var a: i64[>..] = [];
    for i in xs.len {
        inner: for k in 2 { if k == 1 { continue :inner; } }
        a.push(xs[i]);
    }
    if xs.len > 1 { return a[xs.len - 2]; }  // bce:elide (the continue names the inner loop)
    return 0;
}

fn left_early(xs: i64[:]) -> i64 {
    var i = 0;
    var b: i64[>..] = [];
    outer: while i < xs.len {
        for k in 2 { if xs[i] == 7 { break :outer; } }
        b.push(xs[i]);
        i += 1;
    }
    if i < xs.len { return xs[i]; }          // bce:elide (i < xs.len here)
    return b[b.len - 1];                     // bce:keep (b may be empty)
}

fn main() {
    let xs: i64[5] = [1, 3, 5, 7, 9];
    print(skipped_push(xs), " ", whole_push(xs), " ", left_early(xs));
}
