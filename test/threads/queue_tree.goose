// Gust: a tree linked by self-relative references crosses a queue as one
// block (§11.2): its links are offsets, which mean the same at any address,
// so qput copies the array's bytes and the receiver's copy is the same tree,
// with no serializer and no work per node. A queue checks each one sent.
import std;

struct Node { key: i64, left: Node&<u32>?, right: Node&<u32>? }
struct Found { nodes: i64, found: i64, depth: i64 }

fn insert(pool: Node[>..]&, key: i64) {
    if pool.len == 0 { pool.push(Node { key: key }); return; }
    var cur .= pool[0];
    loop {
        if key == cur.key { return; }
        if key < cur.key {
            let l = cur.left;
            if l { cur .= l; continue; }
            cur.left .= pool.push(Node { key: key });
            return;
        }
        let r = cur.right;
        if r { cur .= r; continue; }
        cur.right .= pool.push(Node { key: key });
        return;
    }
}

// How deep key is, following the links; -1 when it is not there.
fn depth_of(pool: Node[>..]&, key: i64) -> i64 {
    guard pool.len > 0 else { return -1; }
    var cur .= pool[0];
    var d = 0;
    loop {
        if key == cur.key { return d; }
        let next = if key < cur.key { cur.left } else { cur.right };
        guard next else { return -1; }
        cur .= next;
        d++;
    }
}

// The keys of n's children, as tens and ones.
fn children(n: Node&) -> i64 {
    var s = 0;
    let l = n.left;
    if l { s += l.key * 10; }
    let r = n.right;
    if r { s += r.key; }
    return s;
}

thread_fn searcher() {
    var tree = qget<Node[>..]>();
    var found = 0;
    var deepest = 0;
    for k in 2000 {
        let d = depth_of(tree, k);
        if d >= 0 { found++; deepest = max(deepest, d); }
    }
    qput(Found { nodes: tree.len, found: found, depth: deepest });
}

fn main() {
    var pool: Node[>..] = [];
    for i in 1000 { insert(pool, i * 7919 % 1000 * 2); }   // the even keys below 2000
    var deepest = 0;
    for k in 2000 { deepest = max(deepest, depth_of(pool, k)); }
    let id = thread_spawn(searcher);
    qput(pool);
    let f = qget<Found>();
    thread_wait(id);
    print(f.nodes, " ", f.found, " ", f.depth == deepest);   // 1000 1000 true

    // Sent and taken back, the copy keeps every link: each key is as deep
    // in it as in the original, and it is a copy, which a change leaves the
    // original's own.
    qput(pool);
    var back = qget<Node[>..]>();
    var same = back.len == pool.len;
    for k in 2000 { same = same && depth_of(back, k) == depth_of(pool, k); }
    back[0].key = -1;
    print(same, " ", pool[0].key, " ", back[0].key);       // true 0 -1

    // A fixed array of linked nodes crosses the same way.
    var three: Node[3] = [Node { key: 2 }, Node { key: 1 }, Node { key: 3 }];
    three[0].left .= &three[1];
    three[0].right .= &three[2];
    qput(three);
    let got = qget<Node[3]>();
    print(children(got[0]));                                // 13
}
