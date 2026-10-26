// Gust: stdlib's slots: generation-checked handles over a reusable pool.
// A released slot is reused by the next allocation, and the old handle no
// longer names it; handles of live allocations keep working; a walk sees
// only live ones.
import std;
import slots;

struct Item { id: i64, count: i64 = 0 }

fn main() {
    reusable var items: slots::Slot<Item>[>..] = [];
    let a = slots::alloc(items, Item { id: 1, .. });
    let b = slots::alloc(items, Item { id: 2, .. });
    slots::get(items, a).count += 5;
    print("a ", slots::get(items, a).id, " ", slots::get(items, a).count);
    slots::release(items, a);
    let c = slots::alloc(items, Item { id: 3, .. });
    print("reused ", c.index == a.index, ", a valid ", slots::valid(items, a), ", c valid ",
          slots::valid(items, c), ", b ", slots::get(items, b).id);
    print("default handle valid ", slots::valid(items, slots::Handle { .. }));
    var live = 0;
    for i in items.len {
        let h = slots::handle_at(items, i);
        if slots::valid(items, h) { live += slots::get(items, h).id; }
    }
    print("live ids sum ", live);
}
