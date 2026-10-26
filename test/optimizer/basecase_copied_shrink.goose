// Modified for Gust (https://github.com/appdoze/gust); see MODIFICATIONS.md.
// Base-case inlining copies the leading base case to the self-call, its
// locals with it: the copy of `r` refers to the copy of `b`, so the shrink
// through it empties that array, and the index after it keeps its check,
// which fires.
recursive fn f(n: i64) -> i64 {
    if n < 1 {
        return {
            var b: i64[>..] = [1, 2, 3];
            let r .= b;
            r.clear();
            b[2]                                    // bce:keep
        };
    }
    return f(n - 1) + 1;
}

fn main() {
    print(f(3));
    // Reaches the original base case too, which otherwise only n < 1 at
    // entry would: the analysis proves 1 <= n at every call of f(3)'s
    // recursion (bce.h, InductiveEntry) and would elide its dead check.
    print(f(0));
}
