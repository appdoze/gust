// error: a reduce adds once an iteration: not in a loop inside the parallel for's body
// In a loop it would add more than once an iteration, in an order the slots do not keep.
fn main() {
    var total = 0;
    parallel for i in 10 {
        for j in i { reduce total += j; }
    }
    print(total);
}
