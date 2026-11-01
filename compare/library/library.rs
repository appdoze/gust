// compare/library's Rust twin (library.goose says what this is), std alone:
// JSON, base64 and regular expressions are Gust's and Go's only.
use std::io::Write;
use std::time::Instant;

struct Rnd { state: u32 }
impl Rnd {
    fn next(&mut self) -> u32 {
        self.state = self.state.wrapping_mul(1664525).wrapping_add(1013904223);
        self.state
    }
    fn value(&mut self) -> f64 { 0.001 + (self.next() as f64) * (1.0 / 4294967296.0) * 1000.0 }
}

const NF: usize = 400000;
const NI: usize = 2000000;

fn fmtf() -> f64 {
    let mut rnd = Rnd { state: 12345 };
    let mut out: Vec<u8> = Vec::new();
    let mut sum = 0usize;
    for _ in 0..NF {
        out.clear();
        write!(out, "{}", rnd.value()).unwrap();
        sum += out.len();
    }
    sum as f64
}

fn parsef() -> f64 {
    let mut rnd = Rnd { state: 12345 };
    let mut text: Vec<u8> = Vec::new();
    for i in 0..NF {
        if i > 0 { text.push(b' '); }
        write!(text, "{}", rnd.value()).unwrap();
    }
    let s = std::str::from_utf8(&text).unwrap();
    let mut sum = 0.0;
    for w in s.split(' ') {
        if let Ok(v) = w.parse::<f64>() { sum += v; }
    }
    sum
}

fn fmti() -> f64 {
    let mut rnd = Rnd { state: 777 };
    let mut out: Vec<u8> = Vec::new();
    let mut sum = 0usize;
    for _ in 0..NI {
        out.clear();
        write!(out, "{}", rnd.next()).unwrap();
        sum += out.len();
    }
    sum as f64
}

fn parsei() -> f64 {
    let mut rnd = Rnd { state: 777 };
    let mut text: Vec<u8> = Vec::new();
    for i in 0..NI {
        if i > 0 { text.push(b' '); }
        write!(text, "{}", rnd.next()).unwrap();
    }
    let s = std::str::from_utf8(&text).unwrap();
    let mut sum = 0i64;
    for w in s.split(' ') {
        if let Ok(v) = w.parse::<i64>() { sum += v; }
    }
    sum as f64
}

const NW: usize = 2000000;

fn make_text() -> String {
    let mut rnd = Rnd { state: 2026 };
    let mut out: Vec<u8> = Vec::new();
    for i in 0..NW {
        if i > 0 { out.push(b' '); }
        write!(out, "w{}", rnd.next() % 50000).unwrap();
    }
    String::from_utf8(out).unwrap()
}

fn find_all() -> f64 {
    let text = make_text();
    let mut total = 0usize;
    for _ in 0..3 {
        let mut sub: &str = &text;
        while let Some(p) = sub.find("w49999 w") {
            total += 1;
            sub = &sub[p + 1..];
        }
    }
    total as f64
}

fn replace_all() -> f64 {
    let text = make_text();
    let mut sum = 0usize;
    for _ in 0..3 {
        let out = text.replace("w1", "wordone");
        sum += out.len();
    }
    sum as f64
}

fn split_join() -> f64 {
    let text = make_text();
    let mut sum = 0usize;
    for _ in 0..3 {
        let mut out = String::new();
        for w in text.split(' ') {
            if !out.is_empty() { out.push(','); }
            out.push_str(w);
        }
        sum += out.len();
    }
    sum as f64
}

fn lower() -> f64 {
    let mut text = make_text().into_bytes();
    let mut sum = 0usize;
    for r in 0..20 {
        text.make_ascii_uppercase();
        text.make_ascii_lowercase();
        sum += text[r] as usize;
    }
    sum as f64
}

fn sort_strings() -> f64 {
    let text = make_text();
    let mut words: Vec<&str> = text[..text.len() / 4].split(' ').collect();
    words.sort_unstable();
    (words[0].len() + words[words.len() - 1].len() + words.len()) as f64
}

fn sort_ints() -> f64 {
    let mut rnd = Rnd { state: 7 };
    let mut xs: Vec<i64> = Vec::new();
    for _ in 0..3000000 { xs.push(rnd.next() as i64); }
    xs.sort_unstable();
    (xs[0] + xs[xs.len() / 2]) as f64
}

fn push_ints() -> f64 {
    let mut sum = 0i64;
    for _ in 0..3 {
        let mut xs: Vec<i64> = Vec::new();
        for i in 0..20000000 { xs.push(i); }
        sum += xs[xs.len() - 1];
    }
    sum as f64
}

fn file_io() -> f64 {
    let text = make_text();
    let mut sum = 0usize;
    for _ in 0..5 {
        std::fs::write("gap_file.tmp", &text).unwrap();
        let back = std::fs::read("gap_file.tmp").unwrap();
        sum += back.len();
    }
    std::fs::remove_file("gap_file.tmp").unwrap();
    sum as f64
}

fn main() {
    let only = std::env::args().nth(2).unwrap_or_default();
    let benches: [(&str, fn() -> f64); 12] = [("fmtf", fmtf), ("parsef", parsef), ("fmti", fmti), ("parsei", parsei),
        ("find", find_all), ("replace", replace_all), ("splitjoin", split_join), ("lower", lower),
        ("sortstr", sort_strings), ("sortint", sort_ints), ("push", push_ints), ("fileio", file_io)];
    for (name, f) in benches {
        if !only.is_empty() && only != name { continue; }
        let t0 = Instant::now();
        let ck = f();
        println!("bench\t{}\t{}\t{}", name, t0.elapsed().as_secs_f64(), ck);
    }
}
