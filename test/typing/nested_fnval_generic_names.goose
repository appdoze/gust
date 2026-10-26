// A nested function of a generic, passed as a function value into another
// generic with a type parameter of the same name: the names in its
// parameter types are its own function's bindings, not the callee's (Gust).

struct Box<S> { app: S, n: i64 = 0 }

// S here is the whole Box, where outer's S is what the Box holds.
fn call_both<D, P, S>(v: S) -> S {
    var x = v;
    D(x);
    P(x);
    return x;
}

fn outer<S>(proto: S) -> Box<S> {
    var calls = 0;
    fn first(b: Box<S>&) { b.n += 1; calls++; }
    fn second(b: Box<S>&) { b.n += 10; calls++; }
    var r = call_both(Box { app: proto, .. }, first, second);
    r.n += calls * 100;
    return r;
}

fn main() {
    let a = outer(5);
    print(a.app, " ", a.n);
    let b = outer("text");
    print(b.app, " ", b.n);
}
