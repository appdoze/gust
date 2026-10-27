// error: no loop or block around this break is labeled rows
// A labeled break names a loop or block it is inside (Gust, §6.4).
fn main() {
    cols: for x in 3 {
        if x == 1 { break :rows; }
    }
}
