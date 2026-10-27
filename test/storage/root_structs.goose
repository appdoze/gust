// Root structs (docs/design/root_structs.md): one declaration holds several
// resizable fields, each stored apart on a data stack of its own, beside
// the struct of the other fields. A field is reached through the owner or a
// `R&` parameter, as `x.field`, and methods take the owner by reference.

struct Token { kind: u8, start: i64 }

root struct ParserState {
    tokens: Token[>..] = [],
    work: i64[>..<] = [],
    position: i64 = 0
}

fn parse(state: ParserState&, input: u8[:]) {
    for c, i in input {
        if c == ' ' { continue; }
        state.tokens.push(Token { kind: c, start: i });
        state.work.push(i);
        state.position += 1;
    }
}

fn count(state: const ParserState&) -> i64 {
    let s: const ParserState& = state;    // a binding, typed: the same fields
    return s.tokens.len + s.work.len;
}

// A generic root: a slot pool of its items and a free list.
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

recursive fn fill(lx: Lexer&, n: i64) {
    if n == 0 { return; }
    lx.add("w");
    fill(lx, n - 1);
}

var order: i64[>..] = [];
fn note(k: i64) -> i64 { order.push(k); return k; }

root struct Ordered {
    first: i64 = 0,
    items: i64[>..] = [],
    last: i64 = 0
}

// A global owner: each field a global of its own.
var global_lexer = Lexer { name: "global", .. };

fn main() {
    var parser = ParserState { .. };
    parse(parser, "a b c");
    let first .= parser.tokens[0];
    parser.work.pop();          // a shrink of work: first, in tokens, stays valid
    parser.parse("de");
    print(parser.position, " ", count(parser), " ", first.kind, " ", parser.work);

    // A reference the pool handed out survives the free list's shrinking;
    // a freed slot's next allocation is what it then reads.
    var pool = slot_pool<i64> { .. };
    let a .= pool.alloc_ref(1);
    let b .= pool.alloc_ref(2);
    pool.free(pool.items.index_of(a));
    let c .= pool.alloc_ref(3);
    b += 10;
    print(a, " ", b, " ", c, " ", pool.items, " ", pool.free_slots.len);
    var names = slot_pool<u8[..8]> { .. };
    let one .= names.alloc_ref("one");
    names.alloc_ref("two");
    names.free(names.items.index_of(one));
    print(names.alloc_ref("three"), " ", names.items.len);

    var lx = Lexer { .. };
    lx.add("ab");
    add(lx, "cde");
    fill(lx, 3);
    print(lx.text, " ", lx.spans.len, " ", lx.spans[1].lo, "-", lx.spans[1].hi, " ", lx.name);

    // A binding names the same fields; a var binding rebinds them all.
    var other = Lexer { line: 9, .. };
    var cur .= lx;
    cur.add("!");
    cur .= other;
    cur.add("x");
    print(lx.text, " ", other.text, " ", cur.line);

    // A nested function reaches the fields as it reaches any local.
    fn total() -> i64 { return lx.text.len + other.text.len; }
    var sum = 0;
    for sp in lx.spans { sum += sp.hi - sp.lo; }
    print(total(), " ", sum);

    // Positional, and the initializers run in the order declared.
    var o = Ordered { note(1), [note(2)], note(3) };
    print(o.first, " ", o.items, " ", o.last, " ", order);

    global_lexer.add("g");
    add(global_lexer, "h");
    print(global_lexer.text, " ", global_lexer.name, " ", global_lexer.spans.len);
}
