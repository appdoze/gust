// runtime-define: GS_DEBUG=1
// Gust: in a debug build, a write through a reference into a reusable pool
// after its slot was freed aborts, reused or not. Expected to abort.
struct Item { id: i64 }

fn main() {
    reusable var pool: Item[>..] = [];
    let a = pool.alloc_index(Item { 1 });
    let r .= pool[a];
    pool.free(a);
    print("freed");
    r.id = 7;
    print("not reached");
}
