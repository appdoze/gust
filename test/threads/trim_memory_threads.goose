// runtime-debug
import os;

var document: u8[>..] = [];

recursive fn trim_helper(depth: i64) {
    if depth > 0 { trim_helper(depth - 1); return; }
    assert(trim_memory());
}

thread_fn trim_worker() {
    var data: u8[>..] = []; data.resize(2 * 1024 * 1024, 53);
    trim_helper(2);
    for c in data { assert(c == 53); }
    for c in document { assert(c == 41); }
    data.clear(); assert(trim_memory());
    qput(42);
}

fn main() {
    document.resize(2 * 1024 * 1024, 41);
    let worker = thread_spawn(trim_worker);
    for round in 3 { trim_helper(2); for c in document { assert(c == 41); } }
    assert(qget<i64>() == 42); thread_wait(worker);
    for c in document { assert(c == 41); }
    document.clear(); assert(trim_memory());
    print("trim memory is thread-local");
}
