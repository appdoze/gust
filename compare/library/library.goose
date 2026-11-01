// compare/library: what programs lean on beyond the eight benchmarks of
// main.goose (text forms, searching, sorting, growth, files, and against Go's
// standard library JSON, base64 and regular expressions). The twins are
// library.rs and go/main.go; `library 1 <name>` prints a bench line as
// main.goose does, and compare/ci_measure.py --library measures them.
import std;
import os;
import json;
import base64;
import regex;

struct Rnd { state: u32 }

fn next(r: Rnd&) -> u32 {
    r.state = r.state * 1664525 + 1013904223;
    return r.state;
}

let NF = 400000;
let NI = 2000000;

fn value(r: Rnd&) -> f64 { 0.001 + r.next() * (1.0 / 4294967296.0) * 1000.0 }

fn fmtf() -> f64 {
    var rnd = Rnd { 12345 };
    var out: u8[>..] = [];
    var sum = 0;
    for i in NF {
        out.clear();
        format(out, rnd.value());
        sum += out.len;
    }
    return sum;
}

fn parsef() -> f64 {
    var rnd = Rnd { 12345 };
    var text: u8[>..] = [];
    for i in NF {
        if i > 0 { text.push(' '); }
        format(text, rnd.value());
    }
    var sum = 0.0;
    each_split(text, ' ') {
        let v, ok = parse_flt(it);
        if ok { sum += v; }
    };
    return sum;
}

fn fmti() -> f64 {
    var rnd = Rnd { 777 };
    var out: u8[>..] = [];
    var sum = 0;
    for i in NI {
        out.clear();
        format(out, rnd.next());
        sum += out.len;
    }
    return sum;
}

fn parsei() -> f64 {
    var rnd = Rnd { 777 };
    var text: u8[>..] = [];
    for i in NI {
        if i > 0 { text.push(' '); }
        format(text, rnd.next());
    }
    var sum = 0;
    each_split(text, ' ') {
        let v, ok = parse_int(it);
        if ok { sum += v; }
    };
    return sum;
}

let NW = 2000000;

fn make_text(out: u8[>..]&) {
    var rnd = Rnd { 2026 };
    for i in NW {
        if i > 0 { out.push(' '); }
        out.append("w");
        format(out, rnd.next() % 50000);
    }
}

fn find_all() -> f64 {
    var text: u8[>..] = [];
    make_text(text);
    var total = 0;
    for r in 3 {
        var sub: u8[:] = text;
        while true {
            let p = find(sub, "w49999 w");
            if p < 0 { break; }
            total += 1;
            sub = sub[p + 1..];
        }
    }
    return total;
}

fn replace_all() -> f64 {
    var text: u8[>..] = [];
    make_text(text);
    var out: u8[>..] = [];
    var sum = 0;
    for r in 3 {
        out.clear();
        format_replaced(out, text, "w1", "wordone");
        sum += out.len;
    }
    return sum;
}

fn split_join() -> f64 {
    var text: u8[>..] = [];
    make_text(text);
    var out: u8[>..] = [];
    var sum = 0;
    for r in 3 {
        out.clear();
        each_split(text, ' ') {
            if out.len > 0 { out.push(','); }
            out.append(it);
        };
        sum += out.len;
    }
    return sum;
}

fn lower() -> f64 {
    var text: u8[>..] = [];
    make_text(text);
    var sum = 0;
    for r in 20 {
        to_upper(text);
        to_lower(text);
        sum += text[r];
    }
    return sum;
}

fn sort_strings() -> f64 {
    var text: u8[>..] = [];
    make_text(text);
    var words: u8[:][>..] = [];
    each_split(text[..text.len / 4], ' ') { words.push(it); };
    sort(words) { a, b => compare(a, b) < 0 };
    return words[0].len + words[words.len - 1].len + words.len;
}

fn sort_ints() -> f64 {
    var rnd = Rnd { 7 };
    var xs: i64[>..] = [];
    for i in 3000000 { xs.push(rnd.next()); }
    sort(xs);
    return xs[0] + xs[xs.len / 2];
}

fn push_ints() -> f64 {
    var sum = 0;
    for r in 3 {
        var xs: i64[>..] = [];
        for i in 20000000 { xs.push(i); }
        sum += xs[xs.len - 1];
    }
    return sum;
}

fn file_io() -> f64 {
    var text: u8[>..] = [];
    make_text(text);
    var sum = 0;
    for r in 5 {
        write_file("gap_file.tmp", text);
        var back: u8[>..] = [];
        read_file("gap_file.tmp", back);
        sum += back.len;
    }
    delete_file("gap_file.tmp");
    return sum;
}

let NR = 200000;

fn write_doc(out: u8[>..]&) {
    var rnd = Rnd { 31 };
    out.push('[');
    for i in NR {
        if i > 0 { out.push(','); }
        out.push('{');
        json::format_key(out, "id");
        json::format_int(out, i);
        out.push(',');
        json::format_key(out, "name");
        out.push('"');
        out.append("user");
        format(out, rnd.next() % 100000);
        out.push('"');
        out.push(',');
        json::format_key(out, "score");
        json::format_number(out, rnd.next() * (1.0 / 4294967296.0) * 100.0);
        out.push(',');
        json::format_key(out, "ok");
        json::format_bool(out, i % 3 == 0);
        out.push('}');
    }
    out.push(']');
}

fn jsonwrite() -> f64 {
    var sum = 0;
    var out: u8[>..] = [];
    for r in 3 {
        out.clear();
        write_doc(out);
        sum += out.len;
    }
    return sum;
}

fn jsonparse() -> f64 {
    var text: u8[>..] = [];
    write_doc(text);
    var sum = 0.0;
    for r in 3 {
        var doc: json::Node[>..] = [];
        var decoded: u8[>..] = [];
        let root, err = json::parse(text, doc, decoded);
        if root {
            json::each(root) {
                let v, ok = json::as_f64(json::member(it, "score"));
                if ok { sum += v; }
            };
        }
    }
    return sum;
}

fn b64() -> f64 {
    var data: u8[>..] = [];
    var rnd = Rnd { 5 };
    for i in 8000000 { data.push(rnd.next() as! u8); }
    var sum = 0;
    for r in 3 {
        var enc: u8[>..] = [];
        var dec: u8[>..] = [];
        base64::encode(enc, data);
        base64::decode(dec, enc);
        sum += enc.len + dec.len;
    }
    return sum;
}

fn re() -> f64 {
    var text: u8[>..] = [];
    var rnd = Rnd { 2026 };
    for i in 1000000 {
        if i > 0 { text.push(' '); }
        text.append("w");
        format(text, rnd.next() % 50000);
    }
    var sum = 0;
    let a = regex::compile("w1[0-9]+ w2[0-9]+");
    let b = regex::compile("[a-z]+49[0-9]*");
    let c = regex::compile("(w12|w34|w56)[0-9] ");
    sum += regex::count(a, text);
    sum += regex::count(b, text);
    sum += regex::count(c, text);
    return sum;
}

fn run<F>(name: u8[:], only: u8[:]) {
    if only.len > 0 && only != name { return; }
    let t0 = clock();
    let checksum = F();
    print("bench\t", name, "\t", clock() - t0, "\t", checksum);
}

fn main() {
    let a = args();
    var only = "";
    if a.len > 2 { only = a[2]; }
    run("fmtf", only, fmtf);
    run("parsef", only, parsef);
    run("fmti", only, fmti);
    run("parsei", only, parsei);
    run("find", only, find_all);
    run("replace", only, replace_all);
    run("splitjoin", only, split_join);
    run("lower", only, lower);
    run("sortstr", only, sort_strings);
    run("sortint", only, sort_ints);
    run("push", only, push_ints);
    run("fileio", only, file_io);
    run("jsonwrite", only, jsonwrite);
    run("jsonparse", only, jsonparse);
    run("base64", only, b64);
    run("regex", only, re);
}
