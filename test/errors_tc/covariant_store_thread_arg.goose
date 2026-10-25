// error: thread_fn parameters must be flat
// Gust regression for the shape of rust-lang/rust#164250: a short-lived
// reference must not reach a longer-lived holder through this channel.
// A holder of a reference passed as a worker's argument.
struct Q { r: i64? }
thread_fn worker(q: Q) { if q.r { print(q.r); } }
fn main() {
    var l = 1;
    let id = thread_spawn(worker, Q { r: l });
    thread_wait(id);
}
