// Fuzzing the JSON parser (stdlib/json.goose) with the text an API server
// reads from its clients: valid documents (nesting, every escape, surrogate
// pairs, numbers on the fast and the exact rounding paths, big and tiny
// exponents) mutated at random, by bytes and by JSON's own tokens. For each
// text: the tree parser and the streaming reader must agree on whether it
// is JSON; a tree must write out as text that parses again and writes out
// the same (strings escaped and decoded back, numbers in their shortest
// form read back exactly); and nothing may abort, since an abort on input
// ends a server's worker. Under ASan and UBSan (test/fuzz_untrusted.py
// runs this for millions of texts) an access out of bounds shows too.
//
//   fuzz_json [texts per document] [seed]

import os;
import json;

struct Rnd { s: u64 }
fn next(r: Rnd&) -> u64 {
    r.s ^= r.s << 13;
    r.s ^= r.s >> 7;
    r.s ^= r.s << 17;
    return r.s;
}
fn below(r: Rnd&, n: i64) -> i64 { ((r.next() >> 1) as i64) % n }
fn byte(r: Rnd&) -> u8 { (r.next() >> 56) as u8 }

// A copy of text with one to four random changes.
fn mutate(text: u8[:], out: u8[>..]&, r: Rnd&) {
    out.clear();
    out.append(text);
    let tokens: (const u8[:])[24] = ["\"", "\\", "[", "]", "{", "}", ",", ":", "e", "E", "-", "+", ".", "0",
                                     "9", "1e400", "\\u", "\\ud800", "\\udc00", "true", "null", " ", "ÿ",
                                     "123456789012345678901234567890"];
    for step in 1 + r.below(4) {
        let kind = r.below(7);
        let n = out.len;
        if n == 0 {
            out.append(tokens[r.below(24)]);
            continue;
        }
        let at = r.below(n);
        if kind == 0 {
            out[at] ^= (1 << r.below(8)) as u8;
        } else if kind == 1 {
            out.resize(r.below(n + 1), 0);
        } else if kind == 2 || kind == 3 {
            // A token inserted: the tail moved up to make room.
            let t = tokens[r.below(24)];
            out.resize(n + t.len, 0);
            var i = n + t.len - 1;
            while i >= at + t.len { out[i] = out[i - t.len]; i -= 1; }
            for c, k in t { out[at + k] = c; }
        } else if kind == 4 {
            let len = min(1 + r.below(8), n - at);
            for i in at..n - len { out[i] = out[i + len]; }
            out.resize(n - len, 0);
        } else if kind == 5 {
            let len = 1 + r.below(6);
            let from = r.below(n);
            for i in len {
                if at + i < n && from + i < n { out[at + i] = out[from + i]; }
            }
        } else {
            out[at] = r.byte();
        }
    }
}

// The tree as JSON text, compact.
recursive fn emit(n: json::Node&, out: u8[>..]&) {
    match n.val {
        Null => { out.append("null"); },
        Bool b => { json::format_bool(out, b.v); },
        Int i => { json::format_int(out, i.v); },
        Num x => { json::format_number(out, x.v); },
        Str t => { json::format_string(out, t.s); },
        Arr => {
            out.push('[');
            var c = n.first;
            var first = true;
            while c {
                if !first { out.push(','); }
                first = false;
                emit(c, out);
                c .= c.next;
            }
            out.push(']');
        },
        Obj => {
            out.push('{');
            var c = n.first;
            var first = true;
            while c {
                if !first { out.push(','); }
                first = false;
                json::format_key(out, c.key);
                emit(c, out);
                c .= c.next;
            }
            out.push('}');
        },
    }
}

struct Tally { tried: i64 = 0, accepted: i64 = 0, disagreed: i64 = 0 }

fn trial(text: u8[:], t: Tally&) {
    t.tried += 1;
    var doc: json::Node[>..] = [];
    var decoded: u8[>..] = [];
    let root, msg = json::parse(text, doc, decoded);
    var r = json::reader(text);
    json::skip_value(r);
    let read_ok = json::finish(r);
    if (root != null) != read_ok {
        t.disagreed += 1;
        if t.disagreed <= 3 { print("  parse and reader disagree (parse ", root != null, "): ", text); }
    }
    guard root else { assert(msg.len > 0); return; }
    t.accepted += 1;
    var once: u8[>..] = [];
    emit(root, once);
    var doc2: json::Node[>..] = [];
    var decoded2: u8[>..] = [];
    let root2, msg2 = json::parse(once, doc2, decoded2);
    guard root2 else { print("  written out, it no longer parses: ", once, " (", msg2, ")"); abort("fuzz_json"); }
    var twice: u8[>..] = [];
    emit(root2, twice);
    if twice != once { print("  written out twice, it differs: ", once, " then ", twice); abort("fuzz_json"); }
}

fn main() {
    let a = args();
    var n = 3000;
    var seed = 1;
    if a.len > 1 { let k, ok = parse_int(a[1]); assert(ok && k > 0); n = k; }
    if a.len > 2 { let k, ok = parse_int(a[2]); assert(ok); seed = k; }
    var r = Rnd { (seed as u64) * 0x9E3779B97F4A7C15 + 1 };
    let docs: (const u8[:])[8] = [
        "{\"id\": 7, \"name\": \"widget\", \"price_cents\": 1999, \"rating\": 4.5, \"in_stock\": true, \"tags\": [\"a\", \"b\"]}",
        "[0, -0, 1, -1, 9223372036854775807, -9223372036854775808, 9223372036854775808, 1.5, -2.25e-3, 1E10, 0.1]",
        "{\"s\": \"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u0041\\u00e9\\u20ac\\ud83d\\ude00\", \"empty\": \"\", \"k\\u0000ey\": null}",
        "[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]], {\"a\": {\"b\": {\"c\": {\"d\": []}}}}]",
        "[2.2250738585072011e-308, 4.9e-324, 1.7976931348623157e308, 123456789012345678901234567890, 0.30000000000000004]",
        "{\"item_id\": 3, \"quantity\": 2, \"note\": \"leave at the door\"}",
        "  [ true , false , null , { } , [ ] , \"\" ]  ",
        "[1e400, -1e400, 1e-400, 7.0e+2, 12.5e-1, 100000000000000000000000.0]",
    ];
    var total = Tally { .. };
    var m: u8[>..] = [];
    for d in docs {
        var t = Tally { .. };
        trial(d, t);
        assert(t.accepted == 1 && t.disagreed == 0);
        for k in n {
            mutate(d, m, r);
            trial(m, t);
        }
        total.tried += t.tried;
        total.accepted += t.accepted;
        total.disagreed += t.disagreed;
    }
    print("json: ", total.tried, " texts, ", total.accepted, " accepted, ", total.disagreed,
          " where the parser and the reader disagree");
}
