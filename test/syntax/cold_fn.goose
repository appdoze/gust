// `cold fn` (§7.15): a program computes the same with it, whatever the
// function is, and `cold` is still a name everywhere else.
import std;

cold fn rare(x: i64) -> i64 { x * 2 + 1 }

cold fn rare_generic<T>(x: T) -> T { x + x }

cold recursive fn countdown(n: i64) -> i64 {
    if n == 0 { return 0; }
    return 1 + countdown(n - 1);
}

fn caller(x: i64) -> i64 {
    if x < 0 { return rare(x); }
    return x;
}

fn nested_user() -> i64 {
    cold fn inner(a: i64) -> i64 { a + 40 }
    return inner(2);
}

fn main() {
    let cold = 5;
    print(cold + caller(-3));
    print(caller(7));
    print(rare_generic(21));
    print(rare_generic(1.5));
    print(countdown(10));
    print(nested_user());
}
