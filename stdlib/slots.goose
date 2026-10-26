// The Gust standard library: `slots` (docs/stdlib.md): generation-checked
// handles over a `reusable` pool (spec §5.4). A reusable pool reuses a
// freed slot for the next allocation, and an index or reference kept from
// before reads whatever is there now: type-safe, but the wrong value
// (spec §9.4). A handle from this module names one allocation instead, by
// its index and a serial number that no other allocation has; using it once
// that allocation is released aborts, in every build, rather than reaching
// the slot's next owner. Releasing twice aborts as well.
//
//     reusable var items: slots::Slot<Item>[>..] = [];
//     let h = slots::alloc(items, Item { .. });
//     slots::get(items, h).count += 1;
//     slots::release(items, h);
//     slots::get(items, h);              // aborts: a stale handle
//
// A reference taken with `get` is an ordinary reference into the pool and is
// not checked after the release: hold handles, not references, across
// anything that may release. Serial numbers count per thread program, as
// pools do not cross threads (a handle sent to another worker names nothing
// there, and its use aborts as stale unless that worker's pool happens to
// hold the same serial at the same index, which needs the same serial to
// be issued twice: they are 63-bit and only ever increase).
namespace slots;

// One allocation: its index in the pool and its serial number. The default
// handle names nothing.
struct Handle { index: i64 = -1, serial: i64 = 0 }

// A pool's element: the value and the serial number of the allocation that
// holds it, 0 while it is free.
struct Slot<T> { serial: i64 = 0, value: T }

// The next serial number of this thread program.
var next_serial = 1;

// A slot holding v, from the pool's freed ones when it has any: its handle.
fn alloc<T>(pool: Slot<T>[>..]&, v: T) -> Handle {
    let s = next_serial;
    next_serial += 1;
    let i = pool.alloc_index(Slot { serial: s, value: v });
    return Handle { index: i, serial: s };
}

// Whether h names an allocation the pool still holds.
fn valid<T>(pool: const Slot<T>[>..]&, h: Handle) -> bool {
    return h.serial != 0 && h.index >= 0 && h.index < pool.len && pool[h.index].serial == h.serial;
}

// The value h names; aborts if it has been released.
fn get<T>(pool: Slot<T>[>..]&, h: Handle) -> T& {
    if !valid(pool, h) { abort("slots: a stale or foreign handle"); }
    return pool[h.index].value;
}

// Gives h's slot back to the pool; aborts if it has been released already.
fn release<T>(pool: Slot<T>[>..]&, h: Handle) {
    if !valid(pool, h) { abort("slots: a stale or foreign handle"); }
    pool[h.index].serial = 0;
    pool.free(h.index);
}

// The handle of the allocation at index i, or the default handle when that
// slot is free: for walking a pool's live allocations.
fn handle_at<T>(pool: const Slot<T>[>..]&, i: i64) -> Handle {
    if i < 0 || i >= pool.len || pool[i].serial == 0 { return Handle { .. }; }
    return Handle { index: i, serial: pool[i].serial };
}
