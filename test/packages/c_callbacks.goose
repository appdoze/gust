// C calling back into Gust (spec §7.11): c_callback(f) is the C address of
// export fn f's wrapper, which C may call while an extern call it is in
// runs. The callback runs on data stacks above what its caller holds, so
// the arrays the caller is building stay intact, and it may grow what lies
// outside it (here a global log), though not shrink it. A callback can call
// C that calls back again.
import std;
import ffi;

extern fn cb_fold(xs: i64[:], f: u64, user: u64) -> i64;
extern fn cb_sort(xs: i64[:], cmp: u64);
extern fn cb_call(f: u64, x: i64) -> i64;
extern fn cb_run(f: u64);

var log: i64[>..] = [];

// Each element as C hands it over, logged, then folded with a scratch array
// of the callback's own on a data stack.
export fn fold_step(acc: i64, x: i64, user: u64) -> i64 {
    log.push(x);
    var scratch: i64[>..] = [];
    for k in 3 { scratch.push(x + k); }
    return acc * (user as i64) + scratch[0] + scratch[2] - scratch[1];
}

fn read_i64(p: u64) -> i64 {
    let b = ffi::bytes(p, 8);
    var v: u64 = 0;
    for i in 8 { v |= (b[i] as u64) << (8 * i); }
    return v as! i64;
}

// qsort's comparator: two pointers into the array C sorts.
export fn by_value(a: u64, b: u64) -> i32 {
    let x = read_i64(a);
    let y = read_i64(b);
    return if x < y { -1 } else if x > y { 1 } else { 0 };
}

// Calls C, which calls back into this same function, until x is 0.
export fn countdown(x: i64) -> i64 {
    if x == 0 { return 0; }
    return 1 + cb_call(c_callback(countdown), x - 1);
}

// What C may run during a call is anything a callback does: here the
// cursor moves, so a check made before the call proves nothing after it.
var cursor = 0;
var table: i64[>..] = [10, 20, 30, 40];
export fn advance() { cursor += 1; }
fn probe() -> i64 {
    if cursor < table.len - 1 {
        cb_run(c_callback(advance));
        return table[cursor]; // bce:keep
    }
    return -1;
}

fn main() {
    // A builder the caller holds across the extern call: the callback's own
    // stacks lie above it, so what is in it stays.
    var before: i64[>..] = [];
    for k in 5 { before.push(k * 11); }
    let xs: i64[>..] = [3, 1, 4, 1, 5];
    let folded = cb_fold(xs, c_callback(fold_step), 10);
    print("folded ", folded, ", logged ", log.len, ", the caller's array intact ",
          before[0] == 0 && before[4] == 44 && before.len == 5);

    var ys: i64[>..] = [42, -7, 19, 0, 7, -100, 3];
    cb_sort(ys, c_callback(by_value));
    var line: u8[>..] = [];
    for y in ys { format(line, y, " "); }
    print("sorted ", line);

    print("countdown through C ", cb_call(c_callback(countdown), 6));
    print("after the cursor moved ", probe(), " ", probe());
}
