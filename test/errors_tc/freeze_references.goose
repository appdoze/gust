// error: freeze copies elements of a fixed size that hold no references (§11.2), not const u8[:]
// A frozen copy is static data: an element holding a reference would lead
// back out of it, into storage that does not live as long.
fn main() {
    let names: const u8[:][2] = ["a", "b"];
    let f = freeze(names);
    print(f.len);
}
