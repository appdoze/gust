// Gust: stdlib's slots: releasing a handle twice aborts, where a plain
// reusable pool's second free(i) would put the slot on its freelist twice
// and hand it to two owners. Expected to abort.
import std;
import slots;

struct Item { id: i64 }

fn main() {
    reusable var items: slots::Slot<Item>[>..] = [];
    let a = slots::alloc(items, Item { 1 });
    slots::release(items, a);
    print("released once");
    slots::release(items, a);
}
