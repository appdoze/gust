// A load at the loop's index from an array shorter than the one it walks
// (src/codegen_lanes.h): the block of lanes ends where that array does, and
// the iterations past it run one at a time, so the index check aborts at
// the first iteration that reads past the end, as the loop does run one
// iteration at a time.
fn main() {
    var xs: f64[>..] = [];
    var ws: f64[>..] = [];
    for i in 11 { xs.push(i * 0.5); }
    for i in 6 { ws.push(1.0 + i); }
    var out: f64[>..] = [];
    for i in 11 { out.push(0.0); }
    for x, i in xs {
        var s = 0.0;
        for k in 3 { s = s + x * ws[i]; }
        out[i] = s;
    }
    print(out);
}
