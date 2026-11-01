// Gust: a thread whose worker has ended may run the next one
// (src/runtime/runtime_threads.h), and the next one cannot tell: its globals,
// data stacks, mailboxes and id are its own, whatever the last one left.
import os;

struct Report { from: i64, seen: i64, total: i64 }

var shared_count: i64 = 7;

fn deep(n: i64) -> i64 {
    var a: i64[>..] = [];
    for i in 1000 { a.push(i + n); }
    var sum = 0;
    for v in a { sum += v; }
    return sum;
}

// Leaves its global changed and its data stack used.
thread_fn dirty(x: i64) {
    let seen = shared_count;
    shared_count = x;
    qput(Report { thread_self(), seen, deep(x) });
}

fn check_globals() {
    var right = 0;
    var last = -1;
    for i in 3000 {
        let id = thread_spawn(dirty, i + 100);
        let r = qget<Report>();
        // A new copy of main's global each time, and an id of its own.
        if r.seen == 7 && r.from == id && id > last && r.total == deep(i + 100) { right++; }
        last = id;
        thread_wait(id);
    }
    print("globals ", right);
}

thread_fn boxed() {
    let v, got = qrecv<i64>(0);
    qput(Report { thread_self(), v, if got { 1 } else { 0 } });
    let w = qrecv<i64>();
    qput(Report { thread_self(), w, 2 });
}

// A message left in a mailbox stays with its id, not with the thread that
// ran it, and the next worker's starts empty.
fn check_mailboxes() {
    var right = 0;
    for i in 200 {
        let id = thread_spawn(boxed);
        let a = qget<Report>();
        qsend(id, i);
        qsend(id, i + 1);          // never taken
        let b = qget<Report>();
        if a.total == 0 && a.seen == 0 && b.seen == i { right++; }
        thread_wait(id);
    }
    print("mailboxes ", right);
}

thread_fn fails(x: i64) {
    if x % 2 == 0 { abort("even"); }
    qput(Report { thread_self(), x, 3 });
}

// A worker that aborts is contained as ever, and those on either side of it run.
fn check_contained() {
    var ok = 0;
    var failed = 0;
    for i in 200 {
        let id = thread_spawn_contained(fails, i);
        if thread_wait(id) {
            ok++;
            if qget<Report>().seen != i { print("wrong report ", i); }
        } else {
            failed++;
        }
    }
    print("contained ", ok, " ", failed);
}

thread_fn nap(ms: i64) {
    sleep_ms(ms);
    qput(Report { thread_self(), ms, 4 });
}

// More at once than wait for work, and started again after they have gone.
fn check_bursts() {
    for round in 2 {
        var ids: i64[>..] = [];
        for i in 300 { ids.push(thread_spawn(nap, 1)); }
        var seen = 0;
        for id in ids { if qget<Report>().seen == 1 { seen++; } }
        for id in ids { thread_wait(id); }
        print("burst ", seen);
        sleep_ms(120);
    }
}

fn main() {
    check_globals();
    check_mailboxes();
    check_contained();
    check_bursts();
    print("main's global ", shared_count);
}
