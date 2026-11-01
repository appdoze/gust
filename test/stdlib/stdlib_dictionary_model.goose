// runtime-debug: stdlib/dictionary.goose against a plain array model.
// Random inserts, updates, get_or_insert, removes, lookups and the odd
// clear or reserve (which then grows a filled table by more than a factor
// of two in place) over a universe of keys, checking every result and the
// count as it goes and every key at the end. Removals from full groups
// leave tombstones, which inserts reuse and growth, or a rehash at the
// same size, clears. The key types: integers; keys whose hashes agree in
// the low 40 bits, so every tag and home group is the same and every probe
// compares keys; keys that all hash to the last group, so each probe
// sequence wraps past the end of the table; slices of every length the
// string hash reads its own way (none, 1-3, 4-8, 9 and more bytes), looked
// up through slices of a second copy of the text; and inline u8[..16]
// keys.
import dictionary;

struct same { v: i64 }
fn hash(k: same) -> u64 { (k.v as! u64) << 40 }

struct last { v: i64 }
fn hash(k: last) -> u64 { (k.v as! u64) << 40 | 1099511627775 }

// One run of `rounds` random operations on keys F(0) .. F(universe - 1);
// true if the dictionary agreed with the model throughout.
fn drive<K, F>(rounds: i64, universe: i64, seed: u64) -> bool {
    var d = dictionary<K, i64> {};
    var present: bool[>..] = [];
    var vals: i64[>..] = [];
    for k in universe {
        present.push(false);
        vals.push(0);
    }
    var r = rng { seed };
    var ok = true;
    var count = 0;
    for step in rounds {
        let k = r.rand_int(universe);
        let op = r.rand_int(100);
        if op < 25 {
            let fresh = d.insert(F(k), step);
            if fresh == present[k] { ok = false; }
            if !present[k] { count++; }
            present[k] = true;
            vals[k] = step;
        } else if op < 40 {
            d.update(F(k), 100) { it += 3; };
            if !present[k] {
                count++;
                present[k] = true;
                vals[k] = 100;
            }
            vals[k] += 3;
        } else if op < 50 {
            let g = d.get_or_insert(F(k), -5);
            if present[k] { if g != vals[k] { ok = false; } } else if g != -5 { ok = false; }
            if !present[k] {
                count++;
                present[k] = true;
                vals[k] = -5;
            }
        } else if op < 75 {
            if d.remove(F(k)) != present[k] { ok = false; }
            if present[k] { count--; }
            present[k] = false;
        } else if op < 99 {
            let g = d.get(F(k));
            if g { if !present[k] || g != vals[k] { ok = false; } } else if present[k] { ok = false; }
            if d.contains(F(k)) != present[k] { ok = false; }
        } else if op == 99 && step % 2 == 0 {
            d.reserve(d.count * 5 + 20);
        } else {
            d.clear();
            count = 0;
            for &p in present { p = false; }
        }
        if d.count != count { ok = false; }
    }
    var total = 0;
    for k in universe {
        let g = d.get(F(k));
        if g { if !present[k] || g != vals[k] { ok = false; } total += g; } else if present[k] { ok = false; }
    }
    var n = 0;
    var sum = 0;
    d.each() { key, v => n++; sum += v; };
    return ok && n == count && sum == total && d.count + d.left <= d.groups.len * 7 && d.left >= 0;
}

// Keys placed by hand: group g is home, v the tag.
struct at { g: i64, v: i64 }
fn hash(k: at) -> u64 { (k.v as! u64) << 57 | (k.g as! u64) }

// Tombstones cleared at the table's own size: in a table of four groups
// (28 entries and tombstones before it grows), ten keys of home 0 fill
// group 0 and spill two into group 1, the next on their probe sequence,
// six of home 1 fill group 1, and eight fill group 2. Removing group 0's
// and group 1's own keys and one of group 2's, all from full groups,
// leaves 15 tombstones; four inserts into group 3 then use up the room,
// and the fifth rehashes the table at its own size (13 entries are at most
// half of 28), which moves the two spilled keys home to group 0.
fn tombstones() -> bool {
    var d = dictionary<at, i64> {};
    d.reserve(28);
    var ok = d.groups.len == 4;
    for v in 10 { d.insert(at { 0, v }, v); }
    for v in 6 { d.insert(at { 1, v }, 100 + v); }
    for v in 8 { d.insert(at { 2, v }, 200 + v); }
    ok = ok && d.count == 24 && d.left == 4;
    for v in 8 { d.remove(at { 0, v }); }
    for v in 6 { d.remove(at { 1, v }); }
    d.remove(at { 2, 0 });
    ok = ok && d.count == 9 && d.left == 4;
    for v in 4 { d.insert(at { 3, v }, 300 + v); }
    ok = ok && d.count == 13 && d.left == 0;
    d.insert(at { 3, 4 }, 304);
    ok = ok && d.groups.len == 4 && d.count == 14 && d.left == 28 - 14;
    for g in 4 {
        for v in 10 {
            let want = (g == 0 && v >= 8) || (g == 2 && v >= 1 && v < 8) || (g == 3 && v < 5);
            let x = d.get(at { g, v });
            if x { if !want || x != g * 100 + v { ok = false; } } else if want { ok = false; }
        }
    }
    // The spilled keys are back in their home group, whose control word
    // the rehash wrote.
    var home = 0;
    for &gr in d.groups[0..1] { for j in 8 { if gr.ctrl >> (j * 8) & 255 < 128 { home++; } } }
    return ok && home == 2;
}

fn main() {
    print(drive<i64>(60000, 3000, 1) { it * 7 - 9000 }, " ",
          drive<i64>(20000, 40, 2) { it });                             // true true
    print(drive<same>(8000, 300, 3) { same { it } }, " ",
          drive<last>(8000, 300, 4) { last { it } });                   // true true

    // Key k is "k", its digits and k % 37 x's; key 0 is empty.
    var text: u8[>..] = [];
    var at: i64[>..] = [0];
    for k in 1..2000 {
        text.push('k');
        format(text, k);
        for j in k % 37 { text.push('x'); }
        at.push(text.len);
    }
    let copy: u8[>..] = copy(text);
    var lens = 0;
    for k in 2000 {
        let s = if k == 0 { text[0..0] } else { text[at[k - 1]..at[k]] };
        if s.len <= 16 { lens++; }
    }
    print(lens, " ", hash(text[0..0]) == hash(copy[0..0]));             // 680 true
    print(drive<u8[:]>(60000, 2000, 5) { if it == 0 { text[0..0] } else { text[at[it - 1]..at[it]] } });  // true
    var same_hashes = true;
    var probe = dictionary<u8[:], i64> {};
    for k in 1..2000 { probe.insert(text[at[k - 1]..at[k]], k); }
    for k in 1..2000 {
        let a = text[at[k - 1]..at[k]];
        let b = copy[at[k - 1]..at[k]];
        if hash(a) != hash(b) { same_hashes = false; }
        let g = probe.get(b);
        if g { if g != k { same_hashes = false; } } else { same_hashes = false; }
    }
    print(same_hashes, " ", probe.count);                                // true 1999

    // The keys of at most 16 bytes, inline.
    var short: i64[>..] = [];
    for k in 1..2000 { if at[k] - at[k - 1] <= 16 { short.push(k); } }
    print(drive<u8[..16]>(40000, short.len, 6) {
        let k = short[it];
        let key: u8[..16] = text[at[k - 1]..at[k]];
        key
    });                                                                  // true
    var inl = dictionary<u8[..16], i64> {};
    for k in short {
        let key: u8[..16] = copy[at[k - 1]..at[k]];
        inl.insert(key, k);
    }
    var found = 0;
    for k in short {
        let key: u8[..16] = text[at[k - 1]..at[k]];
        let a = probe.get(key);
        let b = inl.get(key);
        if a && b { if a == k && b == k { found++; } }
    }
    print(found == short.len, " ", inl.count == short.len, " ", short.len);   // true true 679
    print(tombstones());                                                 // true
}
