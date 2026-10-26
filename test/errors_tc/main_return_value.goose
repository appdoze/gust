// error: fn main() takes no parameters and returns nothing: a return in it has a value
// A main declared without a result cannot get one from a return either (found
// by bench/compiler_fuzz.py: its C main() did not compile).
fn main() {
    print("hi");
    return 5;
}
