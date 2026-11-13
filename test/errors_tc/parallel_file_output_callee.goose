// error: a parallel for's body calls write_file, which changes files, a stream or a socket that every iteration shares; use thread_fn workers
import os;
fn save(i: i64) -> bool {
    var name: u8[>..] = [];
    format(name, "part", i, ".txt");
    return write_file(name, "x");
}
fn main() {
    var ok: bool[10] = [false, false, false, false, false, false, false, false, false, false];
    parallel for i in 10 { ok[i] = save(i); }
}
