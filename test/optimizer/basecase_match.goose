// Gust: base-case inlining's match form (optimize_basecase.h). A recursive
// function whose body is one match over an enum's variants has its leaf
// arms copied to every self-call, with a wildcard arm making the call for
// the rest; the leaves must still run once each, in order, with the
// arguments of their own call.
import std;

enum Expr {
    Const { v: f64 },
    Var { i: i32 },
    Neg { a: Expr..&<u32> },
    Add { a: Expr..&<u32>, b: Expr..&<u32> },
    Mul { a: Expr..&<u32>, b: Expr..&<u32> },
}

var leaves = 0;
var order: i64[>..] = [];

// Leaf arms by reference, one reading a parameter, one a global's count.
recursive fn eval(e: Expr..&, env: f64[:]) -> f64 {
    match e {
        Const &c => { leaves += 1; c.v },
        Var &x => { leaves += 1; order.push(x.i); env[x.i] },
        Neg &n => -eval(n.a, env),
        Add &x => eval(x.a, env) + eval(x.b, env),
        Mul &x => eval(x.a, env) * eval(x.b, env),
    }
}

// A scalar parameter changing from call to call, which each copied leaf must
// see as its own call's.
recursive fn weigh(e: Expr..&, depth: i64) -> i64 {
    return match e {
        Const => depth,
        Var &x => depth * 10 + x.i,
        Neg &n => weigh(n.a, depth + 1),
        Add &x => weigh(x.a, depth + 1) + weigh(x.b, depth + 1),
        Mul &x => weigh(x.a, depth + 1) - weigh(x.b, depth + 1),
    };
}

// Not rewritten: a wildcard arm of its own.
recursive fn count(e: Expr..&) -> i64 {
    match e {
        Neg &n => 1 + count(n.a),
        Add &x => 1 + count(x.a) + count(x.b),
        Mul &x => 1 + count(x.a) + count(x.b),
        _ => 1,
    }
}

// An index out of range in a copied leaf aborts as the callee's would.
recursive fn pick(e: Expr..&, env: f64[:]) -> f64 {
    match e {
        Const &c => c.v,
        Var &x => env[x.i],
        Neg &n => -pick(n.a, env),
        Add &x => pick(x.a, env) + pick(x.b, env),
        Mul &x => pick(x.a, env) * pick(x.b, env),
    }
}

fn main() {
    var pool: Expr..[>..] = [];
    let one: Expr..& = pool.push(Expr.Const { v: 1.5 });
    let x0: Expr..& = pool.push(Expr.Var { i: 0 });
    let x2: Expr..& = pool.push(Expr.Var { i: 2 });
    let neg: Expr..& = pool.push(Expr.Neg { a: x2 });
    let sum: Expr..& = pool.push(Expr.Add { a: one, b: neg });
    let prod: Expr..& = pool.push(Expr.Mul { a: x0, b: sum });
    let top: Expr..& = pool.push(Expr.Add { a: prod, b: x2 });
    let small: Expr..& = pool.push(Expr.Mul { a: x0, b: one });
    var env: f64[3] = [2.0, 7.0, -3.0];
    print("eval ", eval(top, env), ", leaves ", leaves, ", vars in order ", order);
    print("a leaf alone ", eval(x0, env), ", leaves ", leaves);
    print("weigh ", weigh(top, 0), " and of a leaf ", weigh(x2, 4));
    print("count ", count(top));
    var short: f64[2] = [1.0, 2.0];
    print("pick ", pick(small, short));
    print("pick out of range ", pick(top, short));
}
