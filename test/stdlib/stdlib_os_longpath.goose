// Gust: stdlib/os.goose past Windows' old 260-character limit, inside a scratch
// directory the test makes and removes: each call on a relative path that
// grows to about 400 characters, through the line where the runtime changes
// the path's form, and a .. in one that long.
import os;

let DEPTH = 12;

// The directory DEPTH levels down, or fewer.
fn down(root: const u8[:], levels: i64) -> u8[>..] {
    var p: u8[>..] = [];
    p.append(root);
    for i in levels { format(p, "/a_directory_with_a_fairly_long_name_", i); }
    return p;
}

// Removes what an interrupted run left, deepest first.
fn clear(root: const u8[:]) {
    for up in DEPTH + 1 {
        let d = down(root, DEPTH - up);
        var names: u8[>..] = [];
        if list_dir(d, names) {
            each_split(names, '\n') {
                if it.len > 0 && !is_dir(str(d, "/", it)) { delete_file(str(d, "/", it)); }
            };
        }
        delete_dir(d);
    }
}

fn main() {
    let root = "gs_os_longpath_tmp";
    clear(root);
    var made = make_dir(root);
    for i in DEPTH { made = make_dir(down(root, i + 1)) && made; }
    let deep = down(root, DEPTH);
    print(made, " ", is_dir(deep), " ", deep.len > 400);                 // true true true

    let f = str(deep, "/f.txt");
    print(write_file(f, "abc"), " ", append_file(f, "def"), " ", file_exists(f));   // true true true
    var back: u8[>..] = [];
    print(read_file(f, back), " ", back);                                // true abcdef

    let a = str(deep, "/atomic.txt");
    print(write_file_atomic(a, "new"), " ", file_exists(a));             // true true
    let moved = str(deep, "/moved.txt");
    print(rename_file(a, moved), " ", file_exists(a), " ", file_exists(moved));     // true false true

    var names: u8[>..] = [];
    print("list ", list_dir(deep, names), ":");                          // list true:
    write_stdout(names);                                                 // f.txt, moved.txt

    // A .. goes where it would in a short path, written through the parent.
    let viaparent = str(deep, "/../", "a_directory_with_a_fairly_long_name_11/f.txt");
    var again: u8[>..] = [];
    print(read_file(viaparent, again), " ", again);                      // true abcdef

    print(delete_file(f), " ", delete_file(moved), " ", file_exists(f)); // true true false
    clear(root);
    print(is_dir(root));                                                 // false
}
