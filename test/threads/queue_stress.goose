// Gust: typed queues under load from both ends (src/runtime/runtime_threads.h). What
// one sender sends reaches any one receiver in order, and every message arrives once:
// through the ring's growth with senders and receivers running, through the change
// from ring to nodes when another size comes, against a limit, and with receivers
// asleep and being woken one message at a time. Printed values are what must be so,
// whatever the timing.
import os;

struct Stamped { from: i64, seq: i64 }
struct Stop { n: i64 }
struct Tally { count: i64, sum: i64, out_of_order: i64 }

let SENDERS = 3;
let RECEIVERS = 3;

thread_fn stamper(from: i64, count: i64) {
    for i in count { qput(Stamped { from, i }); }
}

// Takes until it is told to stop, a message that counts for nothing.
thread_fn checker(x: i64) {
    var last: i64[8] = [-1; 8];
    var t = Tally { 0, 0, 0 };
    loop {
        let m = qget<Stamped>();
        if m.from < 0 { break; }
        if m.seq <= last[m.from] { t.out_of_order += 1; }
        last[m.from] = m.seq;
        t.count += 1;
        t.sum += m.seq;
    }
    qput(t);
}

fn collect(name: u8[:], expect: i64, expect_sum: i64) {
    var total = Tally { 0, 0, 0 };
    for i in RECEIVERS {
        let t = qget<Tally>();
        total.count += t.count;
        total.sum += t.sum;
        total.out_of_order += t.out_of_order;
    }
    print(name, ": all there ", total.count == expect, ", sum right ", total.sum == expect_sum,
          ", out of order ", total.out_of_order);
}

fn triangle(n: i64) -> i64 { n * (n - 1) / 2 }

// Both ends running from the start, so that the ring grows under them.
fn check_running() {
    let each_count = 200000;
    var ids: i64[>..] = [];
    for i in RECEIVERS { ids.push(thread_spawn(checker, i)); }
    for i in SENDERS { ids.push(thread_spawn(stamper, i, each_count)); }
    for i in SENDERS { thread_wait(ids[RECEIVERS + i]); }
    for i in RECEIVERS { qput(Stamped { -1, 0 }); }
    collect("running", SENDERS * each_count, SENDERS * triangle(each_count));
    for i in RECEIVERS { thread_wait(ids[i]); }
}

// A burst past the size at which a ring gives its memory back, then again, so that
// the receivers empty a ring that goes away, with a sender about to need it.
fn check_bursts() {
    let burst = 70000;
    for round in 4 {
        var ids: i64[>..] = [];
        for i in SENDERS { ids.push(thread_spawn(stamper, i, burst)); }
        for i in RECEIVERS { ids.push(thread_spawn(checker, i)); }
        for i in SENDERS { thread_wait(ids[i]); }
        for i in RECEIVERS { qput(Stamped { -1, 0 }); }
        collect("burst", SENDERS * burst, SENDERS * triangle(burst));
        for i in RECEIVERS { thread_wait(ids[RECEIVERS + i]); }
    }
}

fn stamped_text(n: i64, len: i64) -> u8[>..] {
    var s: u8[>..] = [];
    s.resize(len, (n & 127) as! u8);
    return s;
}

// Text from several senders whose length changes partway, so that what is in the ring
// becomes nodes with both ends at it. One sender's length decides, the rest follow.
thread_fn text_sender(first: i64, count: i64, change_at: i64) {
    for i in count {
        qput(stamped_text(first + i, if i < change_at { 16 } else { 17 + i % 300 }));
    }
}

thread_fn text_receiver(x: i64) {
    var count = 0;
    var wrong = 0;
    loop {
        let s = qget<u8[>..]>();
        if s.len == 1 { break; }
        for c in s { if c != s[0] { wrong += 1; } }
        count += 1;
    }
    qput(Tally { count, 0, wrong });
}

fn check_sizes() {
    let each_count = 20000;
    var ids: i64[>..] = [];
    for i in RECEIVERS { ids.push(thread_spawn(text_receiver, i)); }
    for i in SENDERS { ids.push(thread_spawn(text_sender, i * 1000, each_count, 5000 + i * 3000)); }
    for i in SENDERS { thread_wait(ids[RECEIVERS + i]); }
    for i in RECEIVERS { qput(stamped_text(0, 1)); }
    collect("sizes", SENDERS * each_count, 0);
    for i in RECEIVERS { thread_wait(ids[i]); }
}

struct Bounded { from: i64, seq: i64 }

thread_fn bounded_sender(from: i64, count: i64) {
    var refused = 0;
    for i in count {
        while !qput(Bounded { from, i }) { refused++; }
    }
    qput(Stop { refused });
}

// At most a few wait at once, with senders retrying against the limit.
fn check_limit() {
    let each_count = 50000;
    qlimit<Bounded>(8);
    var ids: i64[>..] = [];
    for i in SENDERS { ids.push(thread_spawn(bounded_sender, i, each_count)); }
    var count = 0;
    var sum = 0;
    var last: i64[8] = [-1; 8];
    var wrong = 0;
    while count < SENDERS * each_count {
        let m = qget<Bounded>();
        if m.seq <= last[m.from] { wrong += 1; }
        last[m.from] = m.seq;
        count += 1;
        sum += m.seq;
    }
    for i in SENDERS {
        qget<Stop>();
        thread_wait(ids[i]);
    }
    print("limit: sum right ", sum == SENDERS * triangle(each_count), ", out of order ", wrong);
}

struct Ring { n: i64 }
struct Back { n: i64 }

thread_fn echoer(rounds: i64) {
    for i in rounds {
        let m = qget<Ring>();
        qput(Back { m.n + 1 });
    }
}

// One message at a time with a pause long enough that the receivers have gone from
// looking to sleeping, so that each message has to wake one.
fn check_sleepers() {
    let rounds = 300;
    var ids: i64[>..] = [];
    for i in RECEIVERS { ids.push(thread_spawn(echoer, rounds)); }
    var right = 0;
    for i in rounds * RECEIVERS {
        if i % 8 == 0 { sleep_ms(1); }
        qput(Ring { i });
        if qget<Back>().n == i + 1 { right += 1; }
    }
    for id in ids { thread_wait(id); }
    print("sleepers: ", right == rounds * RECEIVERS);
}

struct Timed { n: i64 }

thread_fn slow_sender(count: i64) {
    for i in count {
        sleep_ms(2);
        qput(Timed { i });
    }
}

// The timed wait against the same sleep and wake.
fn check_timed() {
    let count = 100;
    let id = thread_spawn(slow_sender, count);
    var got = 0;
    var sum = 0;
    while got < count {
        let m, ok = qget<Timed>(1);
        if ok {
            got += 1;
            sum += m.n;
        }
    }
    thread_wait(id);
    let m, ok = qget<Timed>(1);
    print("timed: sum right ", sum == triangle(count), ", then empty ", !ok, " ", m.n);
}

struct Mail { from: i64, n: i64 }

thread_fn mail_ring(next: i64, rounds: i64) {
    for i in rounds {
        let m = qrecv<Mail>();
        qsend(next, Mail { thread_self(), m.n + 1 });
    }
}

// Mailboxes, whose lookup a thread remembers, around a ring of threads.
fn check_mail() {
    let rounds = 2000;
    let c = thread_spawn(mail_ring, thread_self(), rounds);
    let b = thread_spawn(mail_ring, c, rounds);
    let a = thread_spawn(mail_ring, b, rounds);
    var n = 0;
    for i in rounds {
        qsend(a, Mail { thread_self(), n });
        let m = qrecv<Mail>();
        if m.from != c { print("wrong sender"); }
        n = m.n;
    }
    thread_wait(a);
    thread_wait(b);
    thread_wait(c);
    print("mail: ", n == rounds * 3);
}

fn main() {
    check_running();
    check_bursts();
    check_sizes();
    check_limit();
    check_sleepers();
    check_timed();
    check_mail();
}
