// Gust: labeled break and continue (§6.4). A label names a loop or block
// (`outer: for`), and `break :outer` or `continue :outer` inside it, at any
// depth of nested loops and blocks, exits or continues that one.

import std;

// The first pair of a grid that sums to the target, leaving both loops.
fn find_pair(grid: i64[:], w: i64, target: i64) -> i64, i64 {
    var fx = -1;
    var fy = -1;
    rows: for y in grid.len / w {
        for x in w {
            if grid[y * w + x] + x == target {
                fx = x;
                fy = y;
                break :rows;
            }
        }
    }
    return fx, fy;
}

// Rows with no negative entry, skipping a row at its first.
fn clean_rows(grid: i64[:], w: i64) -> i64 {
    var n = 0;
    rows: for y in grid.len / w {
        for x in w { if grid[y * w + x] < 0 { continue :rows; } }
        n += 1;
    }
    return n;
}

// A loop's value given from inside a nested loop.
fn first_square_above(limit: i64) -> i64 {
    let r = outer: loop {
        for i in 100 {
            if i * i > limit { break :outer i * i; }
        }
        break 0;
    };
    return r;
}

// A labeled block's value, given from a loop inside it, else its tail.
fn index_of(xs: i64[:], v: i64) -> i64 {
    return found: block {
        for x, i in xs { if x == v { break :found i; } }
        -1
    };
}

// While loops: a labeled continue skips the outer loop's step.
fn while_labels(n: i64) -> i64 {
    var i = 0;
    var s = 0;
    outer: while i < n {
        i += 1;
        var j = 0;
        while j < 10 {
            j += 1;
            if j == i { continue :outer; }
            if j * i > 20 { break :outer; }
            s += j;
        }
    }
    return s * 100 + i;
}

// Storage allocated inside the loops a labeled break leaves goes with them:
// a hundred thousand rounds of it would outgrow the data stack otherwise.
fn release_on_exit(rounds: i64) -> i64 {
    var total = 0;
    for r in rounds {
        outer: for a in 3 {
            var xs: i64[>..] = [];
            for b in 3 {
                var ys: i64[>..] = [];
                for k in 8 { ys.push(k + a + b); }
                xs.push(ys[7]);
                if b == 1 { break :outer; }
            }
        }
        total += 1;
    }
    return total;
}

// Definite assignment through the labeled break that leaves both loops.
fn assigned_on_exit(k: i64) -> i64 {
    var x: i64;
    outer: loop {
        loop {
            x = k * 2;
            break :outer;
        }
    }
    return x;
}

// A callee with its own `outer`, inlined into a loop of the same label.
fn inner_search(xs: i64[:]) -> i64 {
    var hits = 0;
    outer: for x in xs {
        for d in 3 { if x == d { hits += 1; continue :outer; } }
    }
    return hits;
}

fn caller(xs: i64[:]) -> i64 {
    var s = 0;
    outer: for r in 3 {
        s += inner_search(xs);
        if r == 1 { break :outer; }
    }
    return s;
}

fn main() {
    let grid: i64[12] = [5, 1, 4, 2, 7, -3, 6, 8, 0, 9, 3, 2];
    let fx, fy = find_pair(grid, 4, 8);
    print(fx, " ", fy);
    print(clean_rows(grid, 4));
    print(first_square_above(50), " ", first_square_above(100000));
    print(index_of(grid, 6), " ", index_of(grid, 42));
    print(while_labels(5));
    print(release_on_exit(100000));
    print(assigned_on_exit(21));
    let xs: i64[5] = [0, 5, 2, 7, 1];
    print(caller(xs));
}
