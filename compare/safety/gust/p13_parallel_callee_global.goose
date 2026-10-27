// Gust: the race hidden in a callee, which counts its calls in a global
// that every iteration's call writes.
var calls = 0;
fn scaled(x: i64) -> i64 {
    calls += 1;
    return x * 3;
}
fn main() {
    var out: i64[100] = [0; 100];
    parallel for i in 100 { out[i] = scaled(i); }
    print(out[99], " ", calls);
}
