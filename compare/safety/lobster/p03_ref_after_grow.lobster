// Keep a reference to element 0, grow the vector, then write through it.
class Cell:
    v:int
let xs = [Cell { 1 }]
let r = xs[0]
for(1000) i: xs.push(Cell { i })
r.v = 99
print xs[0].v
