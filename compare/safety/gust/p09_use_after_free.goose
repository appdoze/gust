// Free a pool slot, let it be reused, then read through the old reference.
struct Item { id: i64 }
fn main() {
    reusable var pool: Item[>..] = [];
    let a = pool.alloc_index(Item { 1 });
    let r .= pool[a];
    pool.free(a);
    let b = pool.alloc_index(Item { 2 });
    print("old reference reads id ", r.id, " (slot reused: ", a == b, ")");
}
