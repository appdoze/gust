// Use a value that may be absent without checking it.
class Node:
    v:int
    next:Node?
let n = Node { 1, nil }
print n.next.v
