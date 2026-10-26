// runtime-define: GS_DEBUG=1
// Gust: in a debug build, a reference into a reusable pool used after its
// slot was freed and reused aborts, where a release build reads the slot's
// new value (compare/safety's probe 9). Expected to abort.
struct Item { id: i64 }

fn main() {
    reusable var pool: Item[>..] = [];
    let a = pool.alloc_index(Item { 1 });
    let r .= pool[a];
    print("before ", r.id);
    pool.free(a);
    let b = pool.alloc_index(Item { 2 });
    print("reused ", a == b);
    print("after ", r.id);
}
