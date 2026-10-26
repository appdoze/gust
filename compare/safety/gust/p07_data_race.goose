// A worker and main both increment the same global counter. Workers get their
// own copy of every global, so there is no shared counter to race on.
var count = 0;
struct Done { n: i64 }
thread_fn worker() {
    for i in 1000000 { count += 1; }
    qput(Done { count });
}
fn main() {
    let id = thread_spawn(worker);
    for i in 1000000 { count += 1; }
    let d = qget<Done>();
    thread_wait(id);
    print("main sees ", count, ", worker saw ", d.n);
}
