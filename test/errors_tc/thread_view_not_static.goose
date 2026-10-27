// error: summer's parameter table is a view, which a worker may be handed only of static data: freeze(a) makes some (§11.2)
// A worker reads a view it is handed in place, so the view must outlive
// every frame of the spawning thread: a local array's does not.
thread_fn summer(table: const i64[:]) { print(table.len); }
fn main() {
    var squares: i64[>..] = [1, 2, 3];
    thread_wait(thread_spawn(summer, squares));
}
