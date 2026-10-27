// error: give root struct Pool's type arguments: Pool<...> { .. }
// A generic root struct's owner names its type arguments: each field's
// declaration needs its type.
root struct Pool<T> { items: T[>..] = [] }
fn main() {
    var p = Pool { .. };
}
