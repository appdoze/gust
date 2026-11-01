// error: extern fn cannot be cold: it has no body to mark
extern cold fn rare() -> i64 = "gs_rare";

fn main() {}
