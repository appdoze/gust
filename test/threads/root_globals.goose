// A worker uses a root struct global as it uses any global (§11.2): through
// its own copy of each field it reaches, taken at the spawn, while a root
// struct it declares itself is its own. Deterministic: main waits for the
// worker before it prints.
root struct Log {
    text: u8[>..] = [],
    marks: i64[>..<] = [],
    count: i64 = 0
}

fn add(lg: Log&, s: const u8[:]) {
    lg.marks.push(lg.text.len);
    lg.text.append(s);
    lg.count += 1;
}

var shared = Log { .. };

thread_fn worker(n: i64) {
    shared.add("w");
    var mine = Log { .. };
    for i in n { mine.add("m"); }
    print("worker ", shared.text, " ", shared.count, " ", mine.text, " ", mine.marks);
}

fn main() {
    shared.add("s");
    let t = thread_spawn(worker, 3);
    thread_wait(t);
    print("main ", shared.text, " ", shared.count, " ", shared.marks);
}
