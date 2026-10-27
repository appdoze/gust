// error: an integer is expected here, got const u8[:]
// qsend's first argument is the receiving thread's id, as thread_spawn
// returned it.
struct Answer { n: i64 }
fn main() {
    qsend("worker", Answer { n: 1 });
}
