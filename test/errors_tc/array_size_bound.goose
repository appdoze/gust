// error: array size 9223372036854775807 exceeds 2^48, the most elements an array can have
// A static capacity past §10.4's bound has no value to fit in (found by
// bench/compiler_fuzz.py: the C compiler refused the type).
fn clip(s: u8[:]) -> u8[..0x7fffffffffffffff] { return s; }
fn main() { print(clip("abc").len); }
