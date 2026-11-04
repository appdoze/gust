// error: a parallel for's body calls request_stop, which changes files, a stream or a socket that every iteration shares; use thread_fn workers
import net;
fn main() {
    parallel for i in 4 { net::request_stop(); }
}
