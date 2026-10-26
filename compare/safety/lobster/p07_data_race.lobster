// A worker and main both increment the same global counter. Every worker is its own VM
// with its own copy of the globals, so there is no shared counter to race on.
class Done:
    attribute serializable
    n:int
var count = 0
if not is_worker_thread():
    start_worker_threads(1)
    for(1000000): count++
    let d = thread_read(typeof Done)
    if d: print "main sees {count}, worker saw {d.n}"
    stop_worker_threads()
else:
    for(1000000): count++
    thread_write(Done { count })
