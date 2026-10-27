// error: a parallel for's body resizes out, which another iteration may also reach: it may write the enclosing code's arrays only at its own index, as out[i]
fn main() {
    var out: i64[>..] = [];
    parallel for i in 10 { out.push(i); }
}
