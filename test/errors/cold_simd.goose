// error: simd fn cannot be cold: its versions are there for speed
simd cold fn rare(x: i64) -> i64 { x }

fn main() {}
