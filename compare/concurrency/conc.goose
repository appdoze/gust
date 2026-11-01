// Threads and messages, against Rust's std and Go's goroutines.
import std;
import os;

struct Ping { n: i64 }
struct Pong { n: i64 }
struct Job { n: i64 }
struct Done { sum: i64 }

let SPAWNS = 2000;
let ROUNDS = 100000;
let JOBS = 1000000;

thread_fn nothing(x: i64) { }

fn spawn_join() -> f64 {
    var sum = 0;
    for i in SPAWNS {
        let id = thread_spawn(nothing, i);
        thread_wait(id);
        sum += 1;
    }
    return sum;
}

fn spawn_many() -> f64 {
    var sum = 0;
    for r in 20 {
        var ids: i64[>..] = [];
        for i in 100 { ids.push(thread_spawn(nothing, i)); }
        for id in ids { thread_wait(id); sum += 1; }
    }
    return sum;
}

thread_fn ponger(from: i64, n: i64) {
    for i in n {
        let p = qrecv<Ping>();
        qsend(from, Pong { n: p.n + 1 });
    }
}

// A worker that does what workers do: it uses a data stack, and sends a result.
thread_fn summer(x: i64) {
    var a: i64[>..] = [];
    for i in 1000 { a.push(i + x); }
    var sum = 0;
    for v in a { sum += v; }
    qput(Done { sum: sum });
}

fn spawn_work() -> f64 {
    var sum = 0;
    for i in SPAWNS {
        let id = thread_spawn(summer, i);
        sum += qget<Done>().sum;
        thread_wait(id);
    }
    return sum;
}

fn pingpong() -> f64 {
    let id = thread_spawn(ponger, thread_self(), ROUNDS);
    var sum = 0;
    for i in ROUNDS {
        qsend(id, Ping { n: i });
        sum += qrecv<Pong>().n;
    }
    thread_wait(id);
    return sum;
}

thread_fn consumer(x: i64) {
    var sum = 0;
    while true {
        let j = qget<Job>();
        if j.n < 0 { break; }
        sum += j.n;
    }
    qput(Done { sum: sum });
}

fn fan_out() -> f64 {
    var ids: i64[>..] = [];
    for w in 3 { ids.push(thread_spawn(consumer, w)); }
    for i in JOBS { qput(Job { n: i }); }
    for w in 3 { qput(Job { n: -1 }); }
    var sum = 0;
    for w in 3 { sum += qget<Done>().sum; }
    for id in ids { thread_wait(id); }
    return sum;
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
    run("spawnjoin", only, spawn_join);
    run("spawnmany", only, spawn_many);
    run("spawnwork", only, spawn_work);
    run("pingpong", only, pingpong);
    run("fanout", only, fan_out);
}
