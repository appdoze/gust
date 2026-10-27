// error: queue elements must be flat (§11.2), or an array linked by self-relative references, not Cell[>..]: i64&<u32>? points at i64, not at an element of the array or one of its variants
// A queue checks an array's links as it is sent, which it can only where
// each link points at an element: one into a field of an element cannot be.
struct Cell { v: i64, at: i64&<u32>? }
fn main() {
    var cells: Cell[>..] = [];
    cells.push(Cell { v: 1, at: null });
    qput(cells);
}
