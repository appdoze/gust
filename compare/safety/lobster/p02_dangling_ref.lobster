// Closest equivalent of returning a reference to a local: Lobster has no references to
// locals, so a function can only hand back a value copy or a reference-counted object.
class Cell:
    x:int
def get() -> Cell:
    let c = Cell { 5 }
    return c
print get().x
