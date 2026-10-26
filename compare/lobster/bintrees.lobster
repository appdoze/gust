// Binary trees, see ../EXTRA_SPEC.md section 6. Every node is a class instance, which is how
// a Lobster program builds a tree; reference counting frees each tree as its last use ends.

private let MIN_DEPTH = 4
private let MAX_DEPTH = 16

private class Node:
    l:Node?
    r:Node?

private def make(depth:int) -> Node:
    if depth == 0: return Node { nil, nil }
    return Node { make(depth - 1), make(depth - 1) }

private def check(n:Node) -> int:
    let l = n.l
    let r = n.r
    if l and r: return 1 + check(l) + check(r)
    return 1

def bench_bintrees() -> float:
    var total = check(make(MAX_DEPTH + 1))
    let long = make(MAX_DEPTH)
    var d = MIN_DEPTH
    while d <= MAX_DEPTH:
        let iterations = 1 << (MAX_DEPTH - d + MIN_DEPTH)
        for(iterations):
            total += check(make(d))
        d += 2
    total += check(long)
    return float(total)
