// Addressed mailboxes (§11.2): a worker asks main through the shared queue,
// carrying its own thread_self(), main answers into that worker's mailbox
// with qsend, and each worker's qrecv takes only the answers sent to it.
struct Ask { from: i64, n: i64 }
struct Answer { n: i64, square: i64 }
struct Total { from: i64, sum: i64, stray: i64 }

thread_fn asker(count: i64) {
    let me = thread_self();
    var sum = 0;
    for n in count {
        qput(Ask { from: me, n: n });
        // Four workers ask at once, and still the answer is to this ask.
        let a = qrecv<Answer>();
        assert(a.n == n);
        sum += a.square;
    }
    // Nothing more was sent to this worker.
    let a, more = qrecv<Answer>(0);
    var stray = 0;
    if more { stray = 1; }
    qsend(-1, Total { from: me, sum: sum, stray: stray });
}

fn main() {
    print(thread_self());                                   // -1
    var ids: i64[>..] = [];
    for w in 4 { ids.push(thread_spawn(asker, 50)); }
    for i in 4 * 50 {
        let q = qget<Ask>();
        qsend(q.from, Answer { n: q.n, square: q.n * q.n });
    }
    var total = 0;
    var stray = 0;
    var from = 0;
    for w in 4 {
        let t = qrecv<Total>();
        total += t.sum;
        stray += t.stray;
        from += t.from;
    }
    for id in ids { thread_wait(id); }
    var spawned = 0;
    for id in ids { spawned += id; }
    print(total, " ", stray, " ", from == spawned);         // 161700 0 true

    // The shared queue and the mailboxes are apart: what qput puts, a
    // qrecv does not take, and a timed qrecv on an empty mailbox gives up.
    qput(Answer { n: 1, square: 1 });
    let r, got = qrecv<Answer>(1);
    let s, shared = qpoll<Answer>();
    print(got, " ", shared, " ", s.square);                 // false true 1
}
