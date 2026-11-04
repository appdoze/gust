// error: a parallel for's body prints, in whichever order the iterations run; print after the loop
fn main() {
    parallel for i in 10 { print(i); }
}
