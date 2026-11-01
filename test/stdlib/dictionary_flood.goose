// Keys chosen to collide, against the hashes as they stand: a table that sees
// them stops trusting its hash and keeps every operation right; a table of
// ordinary keys never does. See dictionary.goose.
import std;
import dictionary;

let K: u64 = 11400714819323198485;

fn inverse(x: u64) -> u64 {
    var y = x;
    for i in 5 { y *= 2 - x * y; }
    return y;
}

// An integer whose hash is t, by undoing hash_mix a step at a time.
fn with_hash(t: u64) -> u64 {
    let u = (t ^ (t >> 32)) * inverse(K);
    return u ^ (u >> 32);
}

fn put64(out: u8[>..]&, v: u64) {
    for i in 8 { out.push(((v >> (8 * i)) & 255) as! u8); }
}

// 24-byte keys with one hash: the second word undoes what the first did to
// the state, and the third is the same in all of them.
fn collide_text(n: i64, out: u8[>..]&) {
    let h0: u64 = 24 * K;
    for i in n {
        let a = i as! u64;
        var h1 = (h0 ^ a) * K;
        h1 = (h1 << 29) | (h1 >> 35);
        put64(out, a);
        put64(out, h1 ^ 0x123456789ABCDEF);
        put64(out, 42);
    }
}

fn check_text(n: i64) {
    var text: u8[>..] = [];
    collide_text(n, text);
    var same = 0;
    for i in n { if text[i * 24..i * 24 + 24].hash() == text[0..24].hash() { same += 1; } }
    var d = dictionary<u8[:], i64> {};
    for i in n { if !d.insert(text[i * 24..i * 24 + 24], i) { print("not new: ", i); } }
    var right = 0;
    for i in n {
        let v = d.get(text[i * 24..i * 24 + 24]);
        if v && v == i { right += 1; }
    }
    print("text: ", same, " of ", n, " with one hash; keyed ", d.seed != 0, "; ", right, " found");
    print("absent: ", d.contains("no such key"));

    // Removed, put back, cleared and refilled, all under the key.
    var removed = 0;
    for i in n { if i % 2 == 0 && d.remove(text[i * 24..i * 24 + 24]) { removed += 1; } }
    var left = 0;
    for i in n { if d.contains(text[i * 24..i * 24 + 24]) { left += 1; } }
    print("removed ", removed, ", left ", left, ", count ", d.count);
    for i in n { d.insert(text[i * 24..i * 24 + 24], i + 1); }
    var sum = 0;
    d.each() { k, v => sum += v; };
    print("put back: count ", d.count, ", sum ", sum);
    d.clear();
    print("cleared: count ", d.count, ", contains ", d.contains(text[0..24]));
    for i in n { d.insert(text[i * 24..i * 24 + 24], i); }
    print("refilled: count ", d.count, ", keyed ", d.seed != 0);
}

// Integers whose hashes agree in the bits that pick the group, by way of
// groups passed where the tags differ and of compares where they agree.
fn check_ints(n: i64, name: u8[:], tags: u64) {
    var d = dictionary<u64, i64> {};
    for i in n {
        let u = i as! u64;
        d.insert(with_hash((u << 24) ^ (u * tags << 57)), i);
    }
    var right = 0;
    for i in n {
        let u = i as! u64;
        let v = d.get(with_hash((u << 24) ^ (u * tags << 57)));
        if v && v == i { right += 1; }
    }
    print(name, ": keyed ", d.seed != 0, "; ", right, " of ", n, " found");
}

// A key of two integers, the second cancelling the first in hash_combine.
struct pair { a: u64, b: u64 }
fn hash(p: pair) -> u64 { hash_combine(hash(p.a), hash(p.b)) }

fn crafted_pair(i: i64) -> pair {
    let a = i as! u64;
    let s = hash(a);
    return pair { a, with_hash((0x5555555555555555 ^ s) - K - (s << 6) - (s >> 2)) };
}

fn check_pairs(n: i64) {
    var same = 0;
    for i in n { if hash(crafted_pair(i)) == hash(crafted_pair(0)) { same += 1; } }
    var d = dictionary<pair, i64> {};
    for i in n { d.insert(crafted_pair(i), i); }
    var right = 0;
    for i in n {
        let v = d.get(crafted_pair(i));
        if v && v == i { right += 1; }
    }
    print("pairs: ", same, " of ", n, " with one hash; keyed ", d.seed != 0, "; ", right, " found");
}

// A key type whose hash itself looks in a table of ordinary keys, which must
// go on finding them while the table asking is under attack.
var salts = dictionary<const u8[:], u64> {};

struct salted { text: u8[:] }
fn hash(k: salted) -> u64 {
    let s = salts.get("salt");
    if !s { abort("the salt went missing"); }
    return hash_combine(k.text.hash(), s);
}

fn check_nested(n: i64) {
    salts.insert("salt", 7);
    salts.insert("pepper", 1);
    var text: u8[>..] = [];
    collide_text(n, text);
    var d = dictionary<salted, i64> {};
    for i in n { d.insert(salted { text[i * 24..i * 24 + 24] }, i); }
    var right = 0;
    for i in n {
        let v = d.get(salted { text[i * 24..i * 24 + 24] });
        if v && v == i { right += 1; }
    }
    print("nested: keyed ", d.seed != 0, ", inner keyed ", salts.seed != 0, "; ", right, " of ", n, " found");
}

fn check_ordinary(n: i64) {
    var r = rng { 99 };
    var d = dictionary<u64, u64> {};
    for i in n { d.insert(r.rand_u64(), 1); }
    var seq = dictionary<i64, i64> {};
    for i in n { seq.insert(i, i); }
    var text: u8[>..] = [];
    for i in n { put64(text, (i as! u64) * 3); }
    var t = dictionary<u8[:], i64> {};
    for i in n { t.insert(text[i * 8..i * 8 + 8], i); }
    print("ordinary: keyed ", d.seed != 0, " ", seq.seed != 0, " ", t.seed != 0);
}

fn main() {
    check_ordinary(200000);
    check_text(20000);
    check_ints(20000, "ints, one tag", 0);
    check_ints(20000, "ints, many tags", 1);
    check_pairs(20000);
    check_nested(2000);
}
