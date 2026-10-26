// Gust: stdlib's slots: the use of a handle after its slot was released and
// reused aborts instead of reading the slot's new owner (what bench's safety
// probe 9 does with a plain reference). Expected to abort.
import std;
import slots;

struct Item { id: i64 }

fn main() {
    reusable var items: slots::Slot<Item>[>..] = [];
    let a = slots::alloc(items, Item { 1 });
    slots::release(items, a);
    let b = slots::alloc(items, Item { 2 });
    print("slot reused: ", a.index == b.index);
    print(slots::get(items, a).id);
}
