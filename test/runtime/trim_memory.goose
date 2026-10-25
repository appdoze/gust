// runtime-debug
import os;

var document: u8[>..] = [];

// Keep this call out of line even at -O2. Every caller must publish its
// cached tops, although this helper mentions none of the caller's arrays.
recursive fn trim_helper(depth: i64) {
    if depth > 0 { trim_helper(depth - 1); return; }
    assert(trim_memory());
}

fn main() {
    var data: u8[>..] = [];
    var limited: u8[..] = [..1048593];
    limited.resize(8197, 91);
    let retained = limited[..]; limited.resize(1);
    for round in 3 {
        data.resize(8 * 1024 * 1024, 73);
        document.resize(2 * 1024 * 1024, 41);
        {
            let live = data[..];
            trim_helper(2);
            for c in live { assert(c == 73); }
            for c in document { assert(c == 41); }
            // The limited array's capacity stays allocated even after a pop.
            for c in retained { assert(c == 91); }
        }
        data.resize(4097); document.clear();
        assert(trim_memory());
        for c in data { assert(c == 73); }
        {
            var scratch: u8[>..] = []; scratch.resize(4 * 1024 * 1024, 67);
            trim_helper(1); for c in scratch { assert(c == 67); }
        }
        trim_helper(1); data.clear();
    }
    assert(trim_memory());
    print("trim memory preserves live values and regrowth");
}
