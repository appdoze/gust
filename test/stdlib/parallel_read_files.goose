// Gust: a parallel for may read files and ask what exists; only changing them
// is refused (test/errors_tc/parallel_file_output.goose). The sizes come back
// the same at any thread count, because nothing in the loop writes a file.
import os;

fn name_of(i: i64) -> u8[>..] { str("gs_parallel_read_tmp_", i, ".txt") }

fn main() {
    let n = 8;
    for i in n {
        var body: u8[>..] = [];
        for k in i + 1 { body.append("x"); }
        write_file(name_of(i), body);
    }
    var sizes: i64[>..] = [];
    sizes.push_n(0, n);
    parallel for i in n {
        var d: u8[>..] = [];
        if file_exists(name_of(i)) && read_file(name_of(i), d) { sizes[i] = d.len; }
    }
    print(sizes);
    for i in n { delete_file(name_of(i)); }
}
