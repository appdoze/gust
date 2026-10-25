// Modified for Gust (https://github.com/appdoze/gust); see MODIFICATIONS.md.
// Gust: data stacks committed in chunks that start at 16 KB and double to
// 1 MB (Windows, macOS; Linux commits on touch). Contents survive growth
// through many chunk boundaries; recursion that grows and releases a stack
// in every frame, at every native depth; a value larger than any chunk, built in one go; and
// workers whose stacks are fresh, each grown past a few chunks, run at once.
import std;

struct Big { head: i64, bytes: u8[300000], tail: i64 }

thread_fn grow(id: i64, n: i64) {
    var s: u8[>..] = [];
    for i in n { s.push(((i + id) % 251) as u8); }
    var sum = 0;
    for i in n { sum += s[i]; }
    qput(sum);
}

// A growable may not be in scope across a recursive call (§7.8), so each
// frame grows and releases its stack before recursing.
recursive fn frame(depth: i64) -> i64 {
    if depth == 0 { return 0; }
    var got = 0;
    {
        var mine: u8[>..] = [];
        mine.resize(4000, (depth % 200) as u8);
        got = mine[3999] as i64 + mine[0];
    }
    return got + frame(depth - 1);
}

fn main() {
    // 40 MB pushed a byte at a time, then checked.
    var s: u8[>..] = [];
    for i in 40000000 { s.push((i % 253) as u8); }
    var bad = 0;
    for i in s.len { if s[i] != (i % 253) as u8 { bad++; } }
    print("grown ", s.len, " bytes, ", bad, " wrong");

    // 2,000 frames, each growing a stack by 4,000 bytes and releasing them.
    var want = 0;
    for d in 1..2001 { want += 2 * (d % 200); }
    print("recursion ", frame(2000) == want);

    // A 300 KB value, default-built, written at both ends.
    var b = Big { head: 1, tail: 2, .. };
    b.bytes[299999] = 9;
    print("big ", b.head, " ", b.bytes[0], " ", b.bytes[299999], " ", b.tail);

    // Sixteen workers at once, each growing a fresh stack by 3 MB.
    var ids: i64[>..] = [];
    for w in 16 { ids.push(thread_spawn(grow, w, 3000000)); }
    var total = 0;
    for w in 16 { total += qget<i64>(); }
    for id in ids { thread_wait(id); }
    var expect = 0;
    for w in 16 {
        for i in 3000000 { expect += (i + w) % 251; }
    }
    print("workers ", total == expect);
}
