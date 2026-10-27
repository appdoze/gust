// error: a parallel for's body uses threads or queues, which the loop's own threads would share
// A parallel for's iterations run on the pool's threads, which have no
// mailboxes of the program's own: thread_self, qsend and qrecv are refused.
fn main() {
    var out: i64[4] = [0, 0, 0, 0];
    parallel for i in 4 {
        out[i] = thread_self();
    }
    print(out[0]);
}
