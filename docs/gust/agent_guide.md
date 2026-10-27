# Gust in one sitting: a guide for AI coding agents

What an agent needs to write Gust that compiles the first time, in one file to
load whole. Each example here is compiled and run by
`scripts/check_doc_examples.py`, so it is current. The full rules are in
[reference.md](../reference.md) and the [spec](../goose_spec.md); this guide
names the section to read when you need more.

## The loop: commands and diagnostics

| Command | What it does |
|---|---|
| `gust check f.gust` | parse and type check only: the fastest feedback |
| `gust run f.gust -- args` | compile and run in process (TinyCC), no build step |
| `gust build f.gust` | a native executable beside the source (clang, gcc or MSVC) |
| `gust format f.gust` | the canonical layout, in place (`--check` to only test) |
| `gust check --diagnostics json f.gust` | errors as JSON: file, line, column, message, related notes |
| `gust help <topic>` | the usage of a command: `run`, `build`, `cimport`, ... |

Read the whole of an error. Lifetime and copy errors name both ends, the
reference and where it was bound, and say what to change. Programs that
start threads need `gust build`; `gust run` refuses them.

## Syntax in brief

```goose
import math;

struct Point { x: f64, y: f64 }

fn norm(p: Point) -> f64 { sqrt(p.x * p.x + p.y * p.y) }   // the last expression is the result

fn main() {
    let a = Point { x: 3.0, y: 4.0 };      // let: not reassigned; types inferred
    var total = 0;                         // var: reassignable, i64 by default
    for i in 5 { total += i; }             // 0..4; `for i in 2..5` is 2, 3, 4
    let kind = if total > 5 { "big" } else { "small" };   // if is an expression
    print(norm(a), " ", total, " ", kind); // print takes any number of values
}
```

```
5.0 10 big
```

- Types follow names (`x: i64`), postfix: `T[>..]`, `T&`, `T?`, `const T[:]`.
- Integer literals adapt to their destination, and are `i64` otherwise.
  Arithmetic is at the operands' width; no promotion to `int`.
- An integer becomes a float implicitly; a float becomes an integer only
  with `x as i64`. Narrowing, a sign change or `u64` to signed needs `as`
  (checked in debug builds) or `as!` (never checked). A cast that changes
  nothing is a warning.
- `x++` and `x += e` are statements, not expressions.
- There are no methods, but `x.f(a)` calls `f(x, a)` for any function.
- Every top-level declaration is visible everywhere in its file, in any order.

## Memory: the five rules

There is no heap, no `malloc`, no garbage collector. Values live in
registers, on the native stack, or on **data stacks** the compiler assigns.

1. **Fixed-size values** (scalars, structs of fixed fields, `T[k]`, `T[..k]`,
   references, slices) copy silently, as in C.
2. **Non-fixed values** (`T[]`, `T[..]`, `T[>..]`, `T[>..<]`, structs holding
   them) are never copied implicitly: `let b = a;` makes `b` another name for
   `a`. Write `copy(a)` for a copy.
3. **A growable array never moves.** A reference into it stays valid however
   much is pushed after.
4. **One growable value per data stack, topmost.** The compiler arranges the
   stacks; you only see it as rules: a struct has at most one resizable
   field and it is the last, and a resizable value is never an array element.
5. **Storage is freed at scope end**, all at once. A reference must not
   outlive the variable that owns its target (section 12 of the reference).

```goose
fn main() {
    var a: i64[>..] = [1, 2, 3];
    var b = copy(a);          // a visible O(n) copy
    b.push(4);
    let c = a;                // no copy: c names a
    c.push(5);
    print(a, " ", b);
}
```

```
[1, 2, 3, 5] [1, 2, 3, 4]
```

## Arrays: pick the kind by what its size does

| Type | Use it for |
|---|---|
| `T[>..]` | the default: a growable array, a string builder, an arena |
| `T[>..<]` | a stack, queue or heap: it also pops and shrinks |
| `T[k]` | a fixed shape: `f64[16]`, a table |
| `T[..k]` | a small inline list or name inside a struct, capacity `k` |
| `T[]` | finished data of a size fixed at construction: a string in a record |
| `T[:]` | a slice: a view, what functions take to accept any array kind |

Members: `.len`, `.push(v)` (returns a reference to the new element),
`.append(slice)`, `.push_n(v, n)`, `.resize(n, v)`, `.clear()`, `.pop()` (on
`T[>..<]`; on `T[>..]` where nothing points in), `xs[i]`, `xs[a..b]`,
`xs[..b]`, `xs[a..]`. Indexing is bounds-checked; the compiler removes the
checks it can prove.

Strings are `u8` arrays: a literal is a `const u8[:]`, a builder is `u8[>..]`,
and `==` compares contents. `str(a, b, ...)` makes a new string,
`format(out, a, b, ...)` appends to one, and `print` writes a line.

```goose
fn shout(s: const u8[:]) -> u8[>..] {
    var out: u8[>..] = [];
    for c in s { out.push(if c >= 'a' && c <= 'z' { c - 32 } else { c }); }
    out.append("!");
    return out;
}

fn main() {
    var words: u8[>..] = [];
    for i in 3 { format(words, "w", i, " "); }
    print(shout("goose"), " ", words.len, " [", words, "]");
}
```

```
GOOSE! 9 [w0 w1 w2 ]
```

An array of variable-size elements (`u8[][>..]`, an array of strings) can be
iterated but not indexed: keep an index of references beside it, or use
`u8[..16][>..]` (fixed-capacity names) when the elements can be capped.

## Structs, enums, references, optionals

```goose
struct Item { id: i32, name: u8[..16], weight: f64 = 1.0 }

enum Shape { Circle { r: f64 }, Rect { w: f64, h: f64 } }

fn area(s: Shape) -> f64 {
    match s {
        Circle c => 3.0 * c.r * c.r,
        Rect r => r.w * r.h,
    }
}

fn heaviest(items: Item[:]) -> Item? {
    var best: Item? = null;
    for &it in items {
        if !best || it.weight > best.weight { best .= it; }
    }
    return best;
}

fn main() {
    var items: Item[>..] = [];
    let first .= items.push(Item { id: 1, name: "bolt", .. });   // `..`: the rest take their defaults
    items.push(Item { id: 2, name: "anvil", weight: 40.0 });
    first.weight = 2.5;                 // a reference writes through
    let h = heaviest(items);
    if h { print(h.name, " ", h.weight); }
    print(area(Shape.Circle { r: 1.0 }), " ", area(Shape.Rect { w: 2.0, h: 3.0 }));
}
```

```
anvil 40.0
3.0 6.0
```

- `T&` is a reference: transparent in expressions, never null. A reference
  parameter (`n: i64&`) binds an lvalue without `&`.
- `let r .= e;` declares a reference to `e`; `r .= e;` rebinds it; plain
  `r = v` writes the target.
- `T?` is a nullable reference. It must be narrowed (`if o`, `while o`,
  `guard o else { ... }`, `assert(o)`) before use.
- `for &x in xs` binds each element by reference; `for x in xs` by value
  (by reference anyway for non-fixed elements); `for x, i in xs` adds the
  index.

## Functions

```goose
fn divide(a: i64, b: i64) -> i64, bool {        // several results
    if b == 0 { return 0, false; }
    return a / b, true;
}

fn largest<T>(xs: T[:]) -> T {                  // generic; `fn f(a, b)` is too
    var m = xs[0];
    for x in xs { if x > m { m = x; } }
    return m;
}

fn apply<F>(x: i64) -> i64 { F(x) }             // F: a function value, passed last
fn twice(x: i64) -> i64 { x * 2 }

fn main() {
    let q, ok = divide(7, 2);
    let xs: f64[3] = [1.5, 9.25, 3.0];
    let thrice = apply(7) { it * 3 };          // a trailing block, its parameter `it`
    print(q, " ", ok, " ", largest(xs), " ", apply(21, twice), " ", thrice);
}
```

```
3 true 9.25 42 21
```

- Parameters are `let`; their contents are as writable as their type. A
  parameter typed `u8[:]` takes a writable or a read-only argument, and a
  write through it is an error at the call that passed a read-only one;
  write `const u8[:]` for a function that only reads.
- Trailing parameters may have defaults: `fn f(a: i64, b: i64 = 0)`.
- Functions overload by parameter types. `sqrt(n)` with an integer `n` is
  ambiguous between `f32` and `f64`: write `sqrt(n as f64)`.
- Nested functions read and write the enclosing function's locals.
- Function values are compile-time: a function name or a trailing block
  passed for a type parameter `F` and called as `F(x)`. They cannot be
  stored or returned, and there are no closures or function pointers.
- **Recursion is opt-in.** Mark the entry of every recursive cycle
  `recursive fn`, write every signature in the cycle in full, and hold no
  growable or variable-size local in scope at a recursive call: put such
  scratch in a block that ends before the call, or own it outside and pass
  a reference or slice in.

```goose
struct Node { v: i64, left: Node&<u32>?, right: Node&<u32>? }

recursive fn total(n: Node?) -> i64 {
    guard n else { return 0; }
    return n.v + total(n.left) + total(n.right);
}

fn main() {
    var pool: Node[>..] = [];                  // the tree's storage, owned outside the recursion
    let root .= pool.push(Node { v: 1 });
    root.left .= pool.push(Node { v: 2 });
    root.right .= pool.push(Node { v: 3 });
    print(total(root));
}
```

```
6
```

`Node&<u32>?` is a relative reference: 4 bytes, an offset from where it is
stored, the way Gust spells a tree in an array (reference section 11).

## Errors you will meet, and the fix

| The error is about | Do this |
|---|---|
| a non-fixed value would be copied | write `copy(x)`, or bind by reference (`let y = x`, a slice parameter) |
| a reference or slice outlives what it points into | declare the holder inside the scope of what it points at, or copy the data out |
| a view of a temporary (`let s = f()[..];`) | name the result first: `let t = f(); let s = t[..];` |
| a recursive call with a growable local in scope | move the local into a block that ends before the call, or own it outside the cycle |
| a function in a recursive cycle without `recursive` or a full signature | mark the cycle's entry `recursive fn` and write every signature in it |
| an array of variable-size elements cannot be indexed | iterate it, keep an index of references, or use a capped element type (`u8[..16]`) |
| a struct holds two resizable fields | keep one resizable field, last; make the others `T[..k]`, `T[]`, or separate variables |
| a write through a read-only parameter or slot | take `const` off where the data is yours, or copy it into a builder |
| ambiguous call (`sqrt(n)`, an integer argument) | convert the argument: `sqrt(n as f64)` |
| redundant `as` (a warning) | delete the cast: the conversion is implicit there |

## Threads and parallel loops

Workers share no memory. A `thread_fn` is compiled as a separate program with
its own copies of the globals, and values cross through typed queues, one per
flat type (no references or slices inside).

<!-- check-only -->
```goose
struct Job { n: i64 }
struct Done { n: i64, square: i64 }

thread_fn worker() {
    loop {
        let j = qget<Job>();
        if j.n < 0 { break; }
        qput(Done { n: j.n, square: j.n * j.n });
    }
}

fn main() {
    let t = thread_spawn(worker);
    for i in 4 { qput(Job { n: i }); }
    qput(Job { n: -1 });
    var sum = 0;
    for i in 4 { sum += qget<Done>().square; }
    thread_wait(t);
    print(sum);
}
```

Whichever thread takes first gets the next value on a shared queue. For an
answer only one worker should get, send its `thread_self()` with the
request: `qsend(id, v)` puts the answer in that thread's own mailbox, and
the worker takes it with `qrecv<T>()`. `main`'s id is `-1`.

`parallel for i in n { ... }` (or `in a..b`) runs iterations on every core
with the result of running them in order, which the checker proves: the body
writes only its own locals and the enclosing code's owned arrays at its own
index (`out[i]`), reads such an array only there, writes no global, and
calls nothing that does. `reduce x += e;` adds into a variable around the
loop, bit for bit as the loop run in order would. `gust run` runs the
iterations in order.

```goose
fn main() {
    let n = 1000;
    var squares: i64[>..] = [];
    squares.push_n(0, n);
    var sum = 0.0;
    parallel for i in n {
        squares[i] = i * i;
        reduce sum += 1.0 / (i + 1);
    }
    var inorder = 0.0;
    for i in n { inorder += 1.0 / (i + 1); }
    print(squares[999], " ", sum == inorder);
}
```

```
998001 true
```

## Modules, packages, the standard library

- `import name;` finds `name.gust` or `name.goose` beside the importing
  file, then the standard library. `namespace ns;` at a file's top puts its
  declarations under `ns::`.
- A package is declared in the root file: `import mylib from
  "github.com/you/mylib@v1";` (a tag, branch or commit), a range
  (`@^1.2`), a registry's name (`@^1.2` alone, with `GUST_REGISTRY` set),
  or a directory (`"../mylib"`). `program.lock` pins what they resolved to.
- Standard modules, each imported by name: `std` (strings, parsing,
  sorting, `find`, `trim`, `sum`, ...), `os` (`args`, `clock`, files,
  environment), `math` (`sqrt`, `floor`, trigonometry), `vec` (`double3`,
  `dot`, `cross`), `dictionary`
  (hash maps), `slots`, `json`, `csv`, `regex`, `base64`, `binary`,
  `exception`, `ffi`, `net` (sockets and an event loop), `http` (a client
  and `http::serve`). [stdlib.md](../stdlib.md) lists every function.

## Calling C

- `extern fn name(a: i64) -> i64;` binds a C function; slices pass their
  data pointer, references their address. `export fn` makes a Gust
  function callable from C, and `c_callback(f)` is its address for C.
- `gust cimport header.h --lib z` writes a package of a header's functions,
  structs and constants, with no `extern fn` to write by hand
  ([reference, C header import](../reference.md#c-header-import-gust)).

## Before you hand code back

1. `gust check` passes with no warnings; `gust format --check` passes.
2. Growable arrays are `T[>..]` unless they must pop (`T[>..<]`) or sit
   inline in a struct (`T[..k]`).
3. Functions that only read take `const` slices.
4. Every recursive cycle has a `recursive fn` entry and holds no growable
   local across its calls.
5. A program with threads is run with `gust build`, not `gust run`.
