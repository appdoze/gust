// error: no loop around this continue is labeled rows
fn main() {
    for x in 3 { continue :rows; }
}
