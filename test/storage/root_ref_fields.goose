// References to a root struct's owner in fields of ordinary structs
// (docs/design/root_structs.md): a field `st: R&` carries a reference to
// each of R's fields stored apart beside it, so a struct holding one is
// copied, returned and stored as any struct of references is. Through a
// name whose type is written -- a parameter or binding declared with the
// struct's type, or one its literal constructs -- `c.st.f` reaches a field
// and `c.st` is passed to an `R&` parameter.

struct Token { kind: u8, start: i64 }

root struct ParserState {
    tokens: Token[>..] = [],
    work: i64[>..<] = [],
    position: i64 = 0
}

struct Ctx { st: ParserState&, depth: i64 = 0 }
struct View { st: const ParserState& }

fn parse(state: ParserState&, input: u8[:]) {
    for c, i in input {
        if c == ' ' { continue; }
        state.tokens.push(Token { kind: c, start: i });
        state.work.push(i);
        state.position += 1;
    }
}

// The owner's fields through the Ctx a parameter is.
fn parse_in(c: Ctx&, input: u8[:]) {
    for ch, i in input {
        if ch == ' ' { continue; }
        c.st.tokens.push(Token { kind: ch, start: i });
        c.st.work.push(i);
        c.st.position += 1;
    }
    c.depth += 1;
}

// A View by value: the copy carries each field's reference.
fn count(v: View) -> i64 {
    let s: const ParserState& = v.st;    // a binding, typed: the same fields
    return s.tokens.len + v.st.work.len;
}

// A generic root, held by a generic struct.
root struct slot_pool<T> {
    items: T[>..] = [],
    free_slots: i64[>..<] = []
}

fn alloc_ref<T>(pool: slot_pool<T>&, v: T) -> T& {
    if pool.free_slots.len > 0 {
        let k = pool.free_slots.pop();
        pool.items[k] = v;
        return pool.items[k];
    }
    return pool.items.push(v);
}

fn free<T>(pool: slot_pool<T>&, k: i64) { pool.free_slots.push(k); }

struct Pools<T> { pool: slot_pool<T>&, label: u8[..8] = "p" }

fn churn<T>(p: Pools<T>, a: T, b: T) {
    p.pool.alloc_ref(a);
    alloc_ref(p.pool, b);
    p.pool.free(0);
    p.pool.alloc_ref(b);
}

struct Span { lo: i64, hi: i64 }

root struct Lexer {
    text: u8[>..] = [],
    spans: Span[>..<] = [],
    line: i64 = 1,
    name: u8[..16] = "anon"
}

fn add(lx: Lexer&, s: const u8[:]) {
    let lo = lx.text.len;
    lx.text.append(s);
    lx.spans.push(Span { lo: lo, hi: lx.text.len });
}

struct LCtx { lx: Lexer&, n: i64 = 0 }

// Through an alias: the type it names.
type LexRef = Lexer&;
struct ACtx { lx: LexRef }

recursive fn fill(lx: Lexer&, n: i64) {
    if n == 0 { return; }
    lx.add("w");
    fill(lx, n - 1);
}

// A recursive function taking the struct that holds the owner.
recursive fn fill_in(c: LCtx, n: i64) {
    if n == 0 { return; }
    c.lx.add("v");
    fill_in(c, n - 1);
}

fn apply<F>(c: LCtx) -> i64 { return F(c); }

// Returned: the result carries the field's references, rooted in lx's.
fn wrap(lx: Lexer&, n: i64) -> LCtx { return LCtx { lx: lx, n: n }; }

var order: i64[>..] = [];
fn note(k: i64) -> i64 { order.push(k); return k; }

// A global owner, and a global holding it.
var global_lexer = Lexer { name: "global", .. };
var global_ctx = LCtx { lx: global_lexer, n: 7 };

fn main() {
    var parser = ParserState { .. };
    var c = Ctx { st: parser };
    parse_in(c, "a b c");
    let first .= c.st.tokens[0];
    c.st.work.push(9);          // a push into work: first, in tokens, stays valid
    parse(c.st, "de");
    c.st.parse("f");
    print(c.st.position, " ", count(View { st: parser }), " ", first.kind, " ", c.st.work, " ", c.depth);

    var pool = slot_pool<i64> { .. };
    let p = Pools<i64> { pool: pool };
    churn(p, 1, 2);
    var names = slot_pool<u8[..8]> { .. };
    let q = Pools<u8[..8]> { pool: names, label: "names" };
    churn(q, "one", "two");
    print(pool.items, " ", p.pool.free_slots.len, " ", q.pool.items, " ", q.label);

    var lx = Lexer { .. };
    let lc = LCtx { lx, 3 };    // positional
    lc.lx.add("ab");
    add(lc.lx, "cde");
    fill(lc.lx, 2);
    fill_in(lc, 2);
    print(lx.text, " ", lc.lx.spans.len, " ", lc.lx.spans[1].lo, "-", lc.lx.spans[1].hi, " ", lc.lx.name, " ",
          lc.n);

    // Copied, returned and stored as an element: each copy reaches the
    // same owner's fields.
    var other = Lexer { line: 9, .. };
    let d: LCtx = lc;
    d.lx.add("!");
    let e: LCtx = wrap(other, 4);
    e.lx.add("x");
    var all: LCtx[>..] = [];
    all.push(lc);
    all.push(e);
    let last: LCtx& = all[1];
    last.lx.add("y");
    var lens = 0;
    for h in all { lens += h.n; }
    print(lx.text, " ", other.text, " ", e.lx.line, " ", all.len, " ", lens);

    // A view into one field is kept across a push into another, through
    // the holder.
    let v .= lc.lx.text[0];
    lc.lx.spans.push(Span { lo: 0, hi: 0 });
    lc.lx.text.push('z');
    print(v, " ", lc.lx.spans.len);

    // Rebinding the field rebinds every field it carries; a binding of it
    // names the same fields.
    var r = LCtx { lx: lx };
    r.lx .= other;
    r.lx.add("+");
    let s = r.lx;
    s.add("-");
    print(other.text, " ", s.line, " ", r.lx.spans.len);

    // A nested function reaches the holder as it reaches any local, and a
    // function value's parameter declared LCtx is a name of known type.
    fn total() -> i64 { return lc.lx.text.len + r.lx.text.len; }
    print(total(), " ", apply(lc) { h: LCtx => h.lx.text.len + h.n });

    global_ctx.lx.add("g");
    add(global_ctx.lx, "h");
    print(global_lexer.text, " ", global_ctx.lx.name, " ", global_ctx.lx.spans.len, " ", global_ctx.n);

    // A binding an `if` chooses among holders' fields.
    var la = Lexer { .. };
    var lb = Lexer { line: 10, .. };
    let ca = LCtx { lx: la };
    let cb = LCtx { lx: lb };
    for i in 4 {
        let w = if note(i) % 2 == 0 { ca.lx } else { cb.lx };
        w.add("x");
        w.line += i;
    }
    print(la.text, " ", la.line, " ", lb.text, " ", lb.line, " ", order);

    let ac = ACtx { lx: la };
    ac.lx.add("!");
    print(la.text, " ", ac.lx.spans.len);
}
