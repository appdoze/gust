// Messages back and forth beside other threads, asleep or busy.
import std;
import os;

struct Ping { n: i64 }
struct Pong { n: i64 }
struct Stop { n: i64 }
struct Done { sum: i64 }

thread_fn ponger(from: i64, n: i64) {
    for i in n {
        let p = qrecv<Ping>();
        qsend(from, Pong { n: p.n + 1 });
    }
}

thread_fn idler(x: i64) {
    let s = qget<Stop>();
    qput(Done { sum: s.n });
}

thread_fn burner(x: i64) {
    var a = x;
    for i in 20000000000 {
        a = (a ^ i) & 1023;
    }
    qput(Done { sum: a });
}

fn pair(rounds: i64) -> f64 {
    let id = thread_spawn(ponger, thread_self(), rounds);
    var sum = 0;
    for i in rounds {
        qsend(id, Ping { n: i });
        sum += qrecv<Pong>().n;
    }
    thread_wait(id);
    return sum;
}

// Sixteen workers with nothing to do, which a count of threads alone would take for a crowd.
fn idle_pair() -> f64 {
    for w in 16 { thread_spawn(idler, w); }
    return pair(100000);
}

// A worker busy on every CPU, which is a crowd.
fn busy_pair() -> f64 {
    for w in hardware_threads() { thread_spawn(burner, w); }
    return pair(2000);
}

fn run<F>(name: u8[:], only: u8[:]) {
    if only.len > 0 && only != name { return; }
    let t0 = clock();
    let checksum = F();
    print("bench\t", name, "\t", clock() - t0, "\t", checksum);
}

fn main() {
    let a = args();
    var only = "";
    if a.len > 2 { only = a[2]; }
    run("idlepair", only, idle_pair);
    run("busypair", only, busy_pair);
}
