// error: a parallel for's body calls append_file, which changes files, a stream or a socket that every iteration shares; use thread_fn workers
import os;
fn main() {
    parallel for i in 10 {
        var line: u8[>..] = [];
        format(line, i, "\n");
        append_file("out.txt", line);
    }
}
