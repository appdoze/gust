// runtime-define: GS_DEBUG=1
// Gust: in a debug build, references into a reusable pool keep their uses
// honest without changing what a correct program does: a reference to one
// slot survives the freeing and reuse of another, a reference rebound after
// its slot was reused reads the new value, one bound after a reuse is fine,
// and a reference passed on is checked where it is read.
struct Item { id: i64, count: i64 = 0 }

fn bump(it: Item&) { it.count += 1; }

fn main() {
    reusable var pool: Item[>..] = [];
    let a = pool.alloc_index(Item { id: 1, .. });
    let b = pool.alloc_index(Item { id: 2, .. });
    var keep .= pool[b];
    pool.free(a);
    let c = pool.alloc_index(Item { id: 3, .. });
    keep.count += 10;
    bump(keep);
    print("kept ", keep.id, " ", keep.count, ", reused ", a == c);

    var r .= pool[c];
    pool.free(c);
    let d = pool.alloc_index(Item { id: 4, .. });
    r .= pool[d];
    print("rebound to ", r.id);

    pool.free(b);
    let e = pool.alloc_index(Item { id: 5, .. });
    let fresh .= pool[e];
    print("bound after the reuse ", fresh.id, ", slot ", e == b);
}
