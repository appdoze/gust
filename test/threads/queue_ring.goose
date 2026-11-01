// Gust: how a queue holds its messages does not show. Small messages of one
// size wait in a ring (src/runtime/runtime_threads.h), and everything else in
// nodes: messages stay in order through the ring's growth, its change to
// nodes when another size comes, and the giving back of a large ring.
import os;

struct Pair { a: i64, b: i64 }
struct Large { first: i64, rest: i64[63], last: i64 }
struct Work { n: i64 }
struct Sum { total: i64 }
struct Woken { n: i64 }
struct Box { n: i64 }

fn in_order(count: i64, from: i64) -> bool {
    for i in from..from + count {
        let p = qget<Pair>();
        if p.a != i || p.b != -i {
            print("out of order at ", i, ": ", p.a, " ", p.b);
            return false;
        }
    }
    return true;
}

// The ring doubles many times from a start in its middle, so that what it
// holds is wrapped around its end when it grows.
fn check_growth() {
    for i in 40 { qput(Pair { i, -i }); }
    print("first ", in_order(30, 0));
    for i in 40..300000 { qput(Pair { i, -i }); }
    print("all ", in_order(300000 - 30, 30));
    let e, got = qpoll<Pair>();
    print("empty ", got, " ", e.a);
    // That ring has been given back, and the next one starts small.
    for round in 3 {
        for i in 100 { qput(Pair { i, -i }); }
        print("again ", in_order(100, 0));
    }
}

fn text(n: i64, len: i64) -> u8[>..] {
    var s: u8[>..] = [];
    s.resize(len, (n & 127) as! u8);
    return s;
}

fn check_text(n: i64, len: i64) -> bool {
    let s = qget<u8[>..]>();
    if s.len != len { return false; }
    for c in s { if c != (n & 127) as! u8 { return false; } }
    return true;
}

// Text of one length waits in a ring until text of another comes, when what
// is waiting becomes nodes, in order, and so does everything after it.
fn check_sizes() {
    for i in 200 { qput(text(i, 16)); }
    var right = 0;
    for i in 50 { if check_text(i, 16) { right++; } }
    qput(text(200, 17));
    for i in 201..300 { qput(text(i, 16)); }
    for i in 50..200 { if check_text(i, 16) { right++; } }
    if check_text(200, 17) { right++; }
    for i in 201..300 { if check_text(i, 16) { right++; } }
    let e, got = qpoll<u8[>..]>();
    print("sizes ", right, " ", got, " ", e.len);
    // An empty message is one of no ring, and neither is a long one.
    qput(text(1, 0));
    qput(text(2, 5000));
    print(check_text(1, 0), " ", check_text(2, 5000));
}

// Too large for a ring from the first.
fn check_large() {
    for i in 100 {
        var m = default<Large>();
        m.first = i;
        m.last = -i;
        for j in 63 { m.rest[j] = i * j; }
        qput(m);
    }
    var right = 0;
    for i in 100 {
        let m = qget<Large>();
        if m.first == i && m.last == -i && m.rest[62] == i * 62 { right++; }
    }
    print("large ", right);
}

fn check_limit() {
    qlimit<Box>(3);
    print(qput(Box { 1 }), " ", qput(Box { 2 }), " ", qput(Box { 3 }), " ", qput(Box { 4 }));
    print(qget<Box>().n, " ", qput(Box { 5 }), " ", qput(Box { 6 }));
    print(qget<Box>().n, " ", qget<Box>().n, " ", qget<Box>().n);
    let e, got = qget<Box>(1);
    print(e.n, " ", got);
}

thread_fn adder() {
    var total = 0;
    loop {
        let w = qget<Work>();
        if w.n < 0 { break; }
        total += w.n;
    }
    qput(Sum { total });
}

thread_fn producer(from: i64, count: i64) {
    for i in count { qput(Work { from + i }); }
    qput(Sum { -1 });
}

// Several on each side, so that receivers race and sleep and are woken.
fn check_threads() {
    let adders = 3;
    let producers = 3;
    let each_count = 200000;
    var ids: i64[>..] = [];
    for i in adders { ids.push(thread_spawn(adder)); }
    for i in producers { ids.push(thread_spawn(producer, i * each_count, each_count)); }
    var finished = 0;
    var total = 0;
    var seen = 0;
    while seen < producers {
        if qget<Sum>().total == -1 { seen++; }
    }
    for i in adders { qput(Work { -1 }); }
    while finished < adders {
        total += qget<Sum>().total;
        finished++;
    }
    for id in ids { thread_wait(id); }
    let n = producers * each_count;
    print("threads ", total == n * (n - 1) / 2);
}

thread_fn sleeper(rounds: i64) {
    for i in rounds {
        // Long enough that the receiver has gone from looking to sleeping.
        sleep_ms(2);
        qput(Woken { i });
    }
}

fn check_wakeups() {
    let rounds = 40;
    let id = thread_spawn(sleeper, rounds);
    var right = 0;
    for i in rounds {
        if i % 2 == 0 {
            if qget<Woken>().n == i { right++; }
        } else {
            let w, got = qget<Woken>(60000);
            if got && w.n == i { right++; }
        }
    }
    thread_wait(id);
    print("wakeups ", right);
}

thread_fn echo() {
    loop {
        let p = qrecv<Pair>();
        if p.a < 0 { break; }
        qsend(-1, Pair { p.a, p.b + 1 });
    }
}

fn check_mailbox() {
    let id = thread_spawn(echo);
    var right = 0;
    for i in 2000 {
        qsend(id, Pair { i, i * 3 });
        let p = qrecv<Pair>();
        if p.a == i && p.b == i * 3 + 1 { right++; }
    }
    qsend(id, Pair { -1, 0 });
    thread_wait(id);
    print("mailbox ", right);
}

fn main() {
    check_growth();
    check_sizes();
    check_large();
    check_limit();
    check_threads();
    check_wakeups();
    check_mailbox();
}
