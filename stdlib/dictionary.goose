// The Goose standard library: `dictionary` (docs/stdlib.md).
// A Swiss table in Go's interleaved layout (Go's internal/runtime/maps,
// after Abseil's): groups of eight slots, each group a control word beside
// its slots, in one power-of-two array. Keys and values are fixed-size;
// strings are keyed as u8[:] slices into text the caller owns, or as inline
// u8[..k]. A user key type provides `fn hash(k: key) -> u64` (and `==`), in
// its own namespace or the global one: `key.hash()` looks in the key type's
// namespace first (spec §11.1). A reference value is an optional one, plain
// or `in pool`: new slots hold default<V>(), which a non-optional reference
// has none of.
//
// Byte j of a group's control word (bits 8j to 8j + 7) is slot j's: 0x80
// when the slot is empty, 0xFE when it is deleted (a tombstone), else the
// top seven bits of its key's hash, independent of the low bits that pick
// the group. A key's probe sequence starts at the group its hash's low bits
// name and steps on by 1, 2, 3, ... groups, which visits every group of a
// power-of-two count. A lookup compares the key's seven bits with all eight
// bytes of a group at once (dict_match), compares keys only where they
// agree, one in 128 of the others, and stops at the first group with an
// empty byte: a group fills up only as entries are added, and stays full
// until a rehash, so a probe never stops short of its key. Removal leaves a
// tombstone only in a full group, where a probe may have gone past it.
//
// The table fills to 7/8, tombstones included, and then doubles in place,
// or rehashes at its own size where tombstones are most of what fills it;
// both clear the tombstones. `left` counts the empty slots that may still
// be filled before that.
//
// A sender who knows the hash can choose keys that all collide, and with
// them make each insert walk past every one before it. So a probe that has
// seen too much, by DICT_FLOOD's measure, marks the table as under attack:
// it gets a random seed (nonzero from then on) and rehashes keyed by it. Until
// then a table costs what it would without this, and its order is the same
// from run to run; Rust and Go pay for a keyed hash on every key instead.
//
// The group array is grow-shrink so that growth can resize it in place
// through a reference (spec §5.2); as a consequence a reference obtained
// from get/get_or_insert, or a binder inside each, may not be in scope at
// the next insert, update, remove or clear (the checker says so).

import std;

const DICT_LSB = 0x0101010101010101;
const DICT_MSB = 0x8080808080808080;
const DICT_EMPTY = 0x80;
const DICT_DELETED = 0xFE;

struct dictionary_slot<K, V> { key: K, val: V }
struct dictionary_group<K, V> { ctrl: u64, slots: dictionary_slot<K, V>[8] }
struct dictionary<K, V> { count: i64 = 0, left: i64 = 0, seed: u64 = 0, groups: dictionary_group<K, V>[>..<] = [] }

// How much of a probe it takes for an insert to call it an attack. A group
// passed counts 1 and a key compared in vain DICT_FLOOD_MISS. Ordinary keys
// stay well short of it: in 40 million probes of tables at their fullest the
// longest passed 36 groups, and a compare fails for one tag in 128. Keys that
// collide outright reach it by the sixteenth, and a table wrong about ordinary
// keys only hashes more slowly from then on.
const DICT_FLOOD = 64;
const DICT_FLOOD_MISS = 4;

// key's hash for a table of this seed. The hash of a key type made of an
// integer can't collide, so the seed mixed in at the end is enough for those;
// the hashes that can, of bytes and of combinations, take it as a key too.
inline fn dict_hash<K>(seed: u64, key: K) -> u64 {
    if (seed | hash_flood_key) != 0 { return dict_hash_keyed(seed, key); }
    return key.hash();
}

// The key is put back after, and cleared for a table of no seed, as a key
// type's hash may itself look something up in another table.
cold fn dict_hash_keyed<K>(seed: u64, key: K) -> u64 {
    let outer = hash_flood_key;
    hash_flood_key = seed;
    let h = key.hash();
    hash_flood_key = outer;
    if seed == 0 { return h; }
    var st = hash_sip_start(seed);
    hash_sip_word(st, h);
    return hash_sip_end(st);
}

// The table is under attack: from now on it is keyed, and every key in it
// goes where that seed puts it. Returns the slot that key has moved to.
cold fn dict_protect<K, V>(d: dictionary<K, V>&, key: K) -> dictionary_slot<K, V>& {
    d.seed = hash_flood_seed() | 1;
    dict_grow(d, d.groups.len);
    let s = dict_find(d, key);
    if s { return s; }
    abort("dictionary: lost a key");
}

// The bytes of control word c that equal the tag t8 holds in each byte,
// each as its high bit: the SWAR zero-byte test on c ^ t8. A byte just above
// a match can show as one too, through the borrow out of the match, which
// the key comparison then turns down; an empty or deleted byte never does.
fn dict_match(c: u64, t8: u64) -> u64 {
    let v = c ^ t8;
    return (v - DICT_LSB) & ~v & DICT_MSB;
}

// The empty bytes of c, each as its high bit: 0x80 is the one byte with
// the high bit set and bit 1 clear.
fn dict_empty(c: u64) -> u64 { c & ~(c << 6) & DICT_MSB }

// The slot of the lowest byte set in a match (m != 0). Its lowest bit,
// shifted down to 1 << 8j, is a multiply that moves byte 7 - j of the
// constant to the top, and that byte's top three bits hold j. It is as
// fast as std's ctz under a C compiler or TinyCC, and under MIR, where a
// call of ctz stays a call, makes the hashmap benchmark 1.24 times faster.
// The `& 7`, which tells bounds-check elimination it is a slot, costs
// nothing: the C compiler sees a 3-bit value.
fn dict_first(m: u64) -> i64 { ((((m & (0 - m)) >> 7) * 0x0020406080A0C0E0 >> 61) as! i64) & 7 }

fn dict_byte(c: u64, j: i64) -> u64 { (c >> (j * 8)) & 255 }

// c with byte j set to b.
fn dict_set(c: u64, j: i64, b: u64) -> u64 {
    let s = j * 8;
    let ff: u64 = 255;
    return (c & ~(ff << s)) | (b << s);
}

// The slot holding key, or null. Like dict_put and remove, it hands back
// the slot where the probe found it, whose group bounds-check elimination
// knows to be one, rather than an index that the caller would check again.
inline fn dict_find<K, V>(d: dictionary<K, V>&, key: K) -> dictionary_slot<K, V>? {
    // An empty table has no groups, which also tells bounds-check
    // elimination that every `& mask` below is a group.
    if d.count == 0 { return null; }
    if d.groups.len == 0 { return null; }
    let h = dict_hash(d.seed, key);
    let t8 = (h >> 57) * DICT_LSB;
    let mask = d.groups.len - 1;
    var g = (h & (mask as u64)) as i64;
    var step = 0;
    loop {
        let c = d.groups[g].ctrl;
        var m = dict_match(c, t8);
        while m != 0 {
            let j = dict_first(m);
            if d.groups[g].slots[j].key == key { return d.groups[g].slots[j]; }
            m &= m - 1;
        }
        if dict_empty(c) != 0 { return null; }
        step += 1;
        g = (g + step) & mask;
    }
}

// The slot holding key, made first if key is absent: the first empty or
// deleted slot on its probe sequence, then holding key and val. The count
// tells the caller which.
fn dict_put<K, V>(d: dictionary<K, V>&, key: K, val: V) -> dictionary_slot<K, V>& {
    dict_room(d);
    // Never taken (dict_room comes first), and with it every `& mask` below
    // is a group that needs no bounds check.
    if d.groups.len == 0 { abort("dictionary: no room"); }
    let h = dict_hash(d.seed, key);
    let t8 = (h >> 57) * DICT_LSB;
    let mask = d.groups.len - 1;
    var g = (h & (mask as u64)) as i64;
    var step = 0;
    var misses = 0;
    // The first slot free on the way, by group and slot.
    var fg = -1;
    var fj = 0;
    loop {
        let c = d.groups[g].ctrl;
        var m = dict_match(c, t8);
        while m != 0 {
            let j = dict_first(m);
            if d.groups[g].slots[j].key == key { return d.groups[g].slots[j]; }
            m &= m - 1;
            misses += 1;
        }
        let free = c & DICT_MSB;
        if free != 0 {
            if fg < 0 {
                fg = g;
                fj = dict_first(free);
            }
            if dict_empty(c) != 0 { break; }
        }
        step += 1;
        g = (g + step) & mask;
    }
    // fg and fj themselves, a group and a slot.
    g = fg & mask;
    let j = fj & 7;
    let c = d.groups[g].ctrl;
    if dict_byte(c, j) == DICT_EMPTY { d.left -= 1; }
    d.groups[g].ctrl = dict_set(c, j, h >> 57);
    d.count += 1;
    d.groups[g].slots[j] = dictionary_slot<K, V> { key, val };
    if step + misses * DICT_FLOOD_MISS >= DICT_FLOOD && d.seed == 0 { return dict_protect(d, key); }
    return d.groups[g].slots[j];
}

// Room for one more entry: the table starts at one group and grows when
// entries and tombstones would fill more than 7/8 of it, to twice its size,
// or to its own size where at most half of what fills it is entries. With
// its three calls dict_grow stays a function of its own rather than being
// inlined into each caller, which keeps the callers small, and the stack
// top that resize fills through stays in a register there
// (docs/implementation.md §6.10).
fn dict_room<K, V>(d: dictionary<K, V>&) {
    if d.left == 0 {
        let n = d.groups.len;
        if (d.count + 1) * 2 <= n * 7 {
            dict_grow(d, n);
        } else {
            dict_grow(d, max(n * 2, 1));
        }
    }
}

// Rehashes the table into n groups, a power of two no smaller than it is,
// in place (hashbrown's rehash_in_place, over an array that may have
// grown). Every entry is marked deleted and every tombstone empty, the
// added groups start empty, and then each marked entry in turn goes to the
// first group on its probe sequence with an empty or deleted byte. Its own
// group has one, so it stays where it is, or moves to an earlier group on
// its sequence: into an empty slot, or swapped with a marked entry that is
// then placed in turn. A placed entry is never moved again, so the groups
// a probe for it passes are all full when the walk ends.
fn dict_grow<K, V>(d: dictionary<K, V>&, n: i64) {
    let old = d.groups.len;
    // 0xFE for a full byte, 0x80 for the others.
    for &g in d.groups { g.ctrl = ((~g.ctrl & DICT_MSB) >> 7) * 0x7E | DICT_MSB; }
    if n > old {
        // Spelled out rather than default<dictionary_group<K, V>>(), so that
        // a V with no default is reported as itself.
        let e = dictionary_slot<K, V> { default<K>(), default<V>() };
        d.groups.resize(n, dictionary_group<K, V> { DICT_MSB, [e; 8] });
    }
    d.left = n * 7 - d.count;
    // The second is never taken (the array only grows), and with it every
    // group index below needs no bounds check.
    if d.count == 0 || d.groups.len < old { return; }
    let mask = d.groups.len - 1;
    for i in old {
        for j in 8 {
            while dict_byte(d.groups[i].ctrl, j) == DICT_DELETED {
                let h = dict_hash(d.seed, d.groups[i].slots[j].key);
                var g = (h & (mask as u64)) as i64;
                var step = 0;
                var free = d.groups[g].ctrl & DICT_MSB;
                while free == 0 {
                    step += 1;
                    g = (g + step) & mask;
                    free = d.groups[g].ctrl & DICT_MSB;
                }
                if g == i {
                    d.groups[i].ctrl = dict_set(d.groups[i].ctrl, j, h >> 57);
                } else {
                    let k = dict_first(free);
                    let was = dict_byte(d.groups[g].ctrl, k);
                    d.groups[g].ctrl = dict_set(d.groups[g].ctrl, k, h >> 57);
                    if was == DICT_EMPTY {
                        d.groups[g].slots[k] = d.groups[i].slots[j];
                        d.groups[i].ctrl = dict_set(d.groups[i].ctrl, j, DICT_EMPTY);
                    } else {
                        let e = d.groups[g].slots[k];
                        d.groups[g].slots[k] = d.groups[i].slots[j];
                        d.groups[i].slots[j] = e;
                    }
                }
            }
        }
    }
}

// A reference to the value for key, or null.
fn get<K, V>(d: dictionary<K, V>&, key: K) -> V? {
    let s = dict_find(d, key);
    if s { return s.val; }
    return null;
}

fn contains<K, V>(d: dictionary<K, V>&, key: K) -> bool {
    if dict_find(d, key) { return true; }
    return false;
}

// Sets key to val; true if the key was new. Like get_or_insert and update,
// it makes room for the key before the one probe that finds it or the slot
// where it goes.
fn insert<K, V>(d: dictionary<K, V>&, key: K, val: V) -> bool {
    let n = d.count;
    let s .= dict_put(d, key, val);
    if d.count != n { return true; }
    // The whole slot, since `=` into a reference V would write its pointee
    // (§3.8); the stored key stays.
    s = dictionary_slot<K, V> { s.key, val };
    return false;
}

// The value for key, inserted as v0 first if absent.
fn get_or_insert<K, V>(d: dictionary<K, V>&, key: K, v0: V) -> V& {
    let s .= dict_put(d, key, v0);
    return s.val;
}

// F(val&) on the value for key, inserted as v0 first if absent.
fn update<K, V, F>(d: dictionary<K, V>&, key: K, v0: V) {
    let s .= dict_put(d, key, v0);
    F(&s.val);
}

// Removes key; true if it was there. The probe is dict_find's, ending at
// the group rather than the slot.
fn remove<K, V>(d: dictionary<K, V>&, key: K) -> bool {
    if d.count == 0 { return false; }
    if d.groups.len == 0 { return false; }
    let h = dict_hash(d.seed, key);
    let t8 = (h >> 57) * DICT_LSB;
    let mask = d.groups.len - 1;
    var g = (h & (mask as u64)) as i64;
    var step = 0;
    loop {
        let c = d.groups[g].ctrl;
        var m = dict_match(c, t8);
        while m != 0 {
            let j = dict_first(m);
            if d.groups[g].slots[j].key == key {
                // A group with an empty byte ends every probe that reaches
                // it, so no probe has gone past this slot to another group
                // and it can be empty again; in a full group it becomes a
                // tombstone, which probes go past.
                if dict_empty(c) != 0 {
                    d.groups[g].ctrl = dict_set(c, j, DICT_EMPTY);
                    d.left += 1;
                } else {
                    d.groups[g].ctrl = dict_set(c, j, DICT_DELETED);
                }
                d.count -= 1;
                return true;
            }
            m &= m - 1;
        }
        if dict_empty(c) != 0 { return false; }
        step += 1;
        g = (g + step) & mask;
    }
}

fn clear<K, V>(d: dictionary<K, V>&) {
    for &g in d.groups { g.ctrl = DICT_MSB; }
    d.count = 0;
    d.left = d.groups.len * 7;
}

// Room for n entries without growing, which a table holding tombstones
// may need a rehash at its own size for.
fn reserve<K, V>(d: dictionary<K, V>&, n: i64) {
    let need = next_pow2((n + 6) / 7);
    if need > d.groups.len || d.left < n - d.count { dict_grow(d, max(need, d.groups.len)); }
}

// F(key, val&) for every entry, in no particular order.
fn each<K, V, F>(d: dictionary<K, V>&) {
    for &g in d.groups {
        var m = ~g.ctrl & DICT_MSB;
        while m != 0 {
            let j = dict_first(m);
            F(g.slots[j].key, &g.slots[j].val);
            m &= m - 1;
        }
    }
}
