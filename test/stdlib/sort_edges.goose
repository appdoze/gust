// Gust: sort(xs) at every small size and in the shapes that have ever caught a sort out: the result in
// order and a permutation of the input, also where < is no strict weak ordering, as with NaN.
import std;

struct Rnd { state: u32 }

fn next(r: Rnd&) -> u32 {
    r.state = r.state * 1664525 + 1013904223;
    return r.state;
}

// How many of each value, as a sum that a lost or doubled element would change.
fn weigh(xs: i64[:]) -> u64 {
    var sum: u64 = 0;
    for x in xs {
        var h = (x as u64) * 0x9E3779B97F4A7C15;
        h = h ^ (h >> 29);
        sum = sum + h * 0xBF58476D1CE4E5B9;
    }
    return sum;
}

fn fill_shape(xs: i64[:], shape: i64, r: Rnd&) {
    let n = xs.len;
    for i in n {
        xs[i] = if shape == 0 { r.next() as i64 }
            else if shape == 1 { i as i64 }
            else if shape == 2 { (n - i) as i64 }
            else if shape == 3 { 7 }
            else if shape == 4 { (r.next() % 4) as i64 }
            else if shape == 5 { (i % 16) as i64 }
            else if shape == 6 { if i % 2 == 0 { i as i64 } else { (n - i) as i64 } }
            else if shape == 7 { if i < n / 2 { i as i64 } else { (i - n / 2) as i64 } }
            else { 0 - (r.next() as i64) };
    }
}

fn check_ints(n: i64, r: Rnd&) -> i64 {
    var failed = 0;
    var xs: i64[>..] = [];
    for i in n { xs.push(0); }
    for shape in 9 {
        for round in 3 {
            fill_shape(xs, shape, r);
            let before = weigh(xs);
            sort(xs);
            if weigh(xs) != before { failed += 1; }
            for i in 1..n {
                if xs[i] < xs[i - 1] { failed += 1; }
            }
        }
    }
    return failed;
}

// With NaN among them a < b says nothing useful: any order will do, but all of them must still be there.
fn check_nans(n: i64, r: Rnd&) -> i64 {
    var failed = 0;
    let nan = 0.0 / (0.0 + (n - n) as f64);
    var xs: f64[>..] = [];
    for i in n { xs.push(0.0); }
    for round in 5 {
        var nans = 0;
        var sum = 0.0;
        for i in n {
            let u = r.next();
            if u % 3 == 0 {
                xs[i] = nan;
                nans += 1;
            } else {
                xs[i] = (u % 1000) as f64;
                sum += xs[i];
            }
        }
        sort(xs);
        var nans_after = 0;
        var sum_after = 0.0;
        for x in xs {
            if x != x { nans_after += 1; } else { sum_after += x; }
        }
        if nans != nans_after || sum != sum_after { failed += 1; }
    }
    return failed;
}

fn check_narrow(n: i64, r: Rnd&) -> i64 {
    var failed = 0;
    var bytes: u8[>..] = [];
    var words: i32[>..] = [];
    var reals: f64[>..] = [];
    for i in n {
        bytes.push((r.next() % 256) as u8);
        words.push((r.next() % 1000) as i32 - 500);
        reals.push((r.next() % 1000) as f64 / 8.0);
    }
    sort(bytes);
    sort(words);
    sort(reals);
    for i in 1..n {
        if bytes[i] < bytes[i - 1] { failed += 1; }
        if words[i] < words[i - 1] { failed += 1; }
        if reals[i] < reals[i - 1] { failed += 1; }
    }
    return failed;
}

fn main() {
    var r = Rnd { 99 };
    var failed = 0;
    for n in 0..200 {
        failed += check_ints(n, r);
        failed += check_nans(n, r);
        failed += check_narrow(n, r);
    }
    for n in [1000, 4097, 65536] {
        failed += check_ints(n, r);
        failed += check_nans(n, r);
        failed += check_narrow(n, r);
    }
    print("failed: ", failed);
}
