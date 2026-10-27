// Gust: the Rust twin of go/rawserver, on tokio: the same minimal HTTP/1.1 as
// the standard library's http server (stdlib/http.goose) (a task per connection, the request parsed in place
// in one buffer, keep-alive and pipelining, the same limits and refusals,
// one write per response) and the same routes, JSON and validation, with
// JSON written and read by hand, as no crate beyond tokio is used. It is what
// Phase 7 of the roadmap measures Gust's server against in Rust.
//
//     cargo build --release && target/release/rawserver_rust [port]

use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};

const MAX_HEAD: usize = 8192;
const MAX_BODY: usize = 8192;
const IDLE: Duration = Duration::from_secs(5);
const POLL: Duration = Duration::from_millis(100);

struct Item {
    id: i64,
    name: &'static str,
    price_cents: i64,
    rating: f64,
    in_stock: bool,
}

const CATALOG: [Item; 8] = [
    Item { id: 1, name: "Espresso", price_cents: 250, rating: 4.6, in_stock: true },
    Item { id: 2, name: "Cappuccino", price_cents: 380, rating: 4.4, in_stock: true },
    Item { id: 3, name: "Flat white", price_cents: 390, rating: 4.7, in_stock: true },
    Item { id: 4, name: "Cold brew", price_cents: 420, rating: 4.1, in_stock: false },
    Item { id: 5, name: "Croissant", price_cents: 310, rating: 4.5, in_stock: true },
    Item { id: 6, name: "Banana bread", price_cents: 295, rating: 3.9, in_stock: true },
    Item { id: 7, name: "Oat milk latte", price_cents: 450, rating: 4.3, in_stock: true },
    Item { id: 8, name: "Matcha", price_cents: 470, rating: 4.0, in_stock: false },
];

static NEXT_ORDER: AtomicI64 = AtomicI64::new(0);
static STOPPING: AtomicBool = AtomicBool::new(false);

fn reason(status: u16) -> &'static str {
    match status {
        200 => "OK",
        201 => "Created",
        400 => "Bad Request",
        404 => "Not Found",
        405 => "Method Not Allowed",
        409 => "Conflict",
        413 => "Content Too Large",
        422 => "Unprocessable Content",
        431 => "Request Header Fields Too Large",
        501 => "Not Implemented",
        _ => "Internal Server Error",
    }
}

// ---------------------------------------------------------------- JSON out

// A string as encoding/json writes it: <, > and & escaped too.
fn json_str(out: &mut Vec<u8>, s: &str) {
    out.push(b'"');
    for c in s.chars() {
        match c {
            '"' => out.extend_from_slice(b"\\\""),
            '\\' => out.extend_from_slice(b"\\\\"),
            '\n' => out.extend_from_slice(b"\\n"),
            '\r' => out.extend_from_slice(b"\\r"),
            '\t' => out.extend_from_slice(b"\\t"),
            '<' | '>' | '&' | '\u{2028}' | '\u{2029}' => out.extend_from_slice(format!("\\u{:04x}", c as u32).as_bytes()),
            c if (c as u32) < 0x20 => out.extend_from_slice(format!("\\u{:04x}", c as u32).as_bytes()),
            c => {
                let mut b = [0u8; 4];
                out.extend_from_slice(c.encode_utf8(&mut b).as_bytes());
            }
        }
    }
    out.push(b'"');
}

fn json_item(out: &mut Vec<u8>, it: &Item) {
    out.extend_from_slice(format!("{{\"id\":{},\"name\":", it.id).as_bytes());
    json_str(out, it.name);
    out.extend_from_slice(
        format!(",\"price_cents\":{},\"rating\":{},\"in_stock\":{}}}", it.price_cents, it.rating, it.in_stock).as_bytes(),
    );
}

fn detail(status: u16, msg: &str) -> (u16, Vec<u8>) {
    let mut b = b"{\"detail\":".to_vec();
    json_str(&mut b, msg);
    b.push(b'}');
    (status, b)
}

// ---------------------------------------------------------------- JSON in

// What an order's body says, as encoding/json reads it into OrderIn: each
// field absent, null or its value; the first type mismatch, by field; or a
// syntax error.
#[derive(Default)]
struct OrderIn {
    item_id: Option<i64>,
    quantity: Option<i64>,
    note: Option<String>,
    mismatch: Option<(&'static str, &'static str)>, // field, Go type
}

struct Reader<'a> {
    s: &'a [u8],
    at: usize,
}

impl<'a> Reader<'a> {
    fn ws(&mut self) {
        while self.at < self.s.len() && matches!(self.s[self.at], b' ' | b'\t' | b'\n' | b'\r') {
            self.at += 1;
        }
    }
    fn peek(&mut self) -> Option<u8> {
        self.ws();
        self.s.get(self.at).copied()
    }
    fn eat(&mut self, c: u8) -> Result<(), String> {
        if self.peek() == Some(c) {
            self.at += 1;
            Ok(())
        } else {
            Err(format!("expected '{}'", c as char))
        }
    }
    fn word(&mut self, w: &[u8]) -> Result<(), String> {
        if self.s[self.at..].starts_with(w) {
            self.at += w.len();
            Ok(())
        } else {
            Err("invalid literal".into())
        }
    }
    fn string(&mut self) -> Result<String, String> {
        self.eat(b'"')?;
        let mut out = String::new();
        loop {
            let c = *self.s.get(self.at).ok_or("unexpected end of JSON input")?;
            self.at += 1;
            match c {
                b'"' => return Ok(out),
                b'\\' => {
                    let e = *self.s.get(self.at).ok_or("unexpected end of JSON input")?;
                    self.at += 1;
                    match e {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'b' => out.push('\u{8}'),
                        b'f' => out.push('\u{c}'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => {
                            let mut cp = self.hex4()?;
                            if (0xd800..0xdc00).contains(&cp) && self.s[self.at..].starts_with(b"\\u") {
                                self.at += 2;
                                let lo = self.hex4()?;
                                if (0xdc00..0xe000).contains(&lo) {
                                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                                }
                            }
                            out.push(char::from_u32(cp).unwrap_or('\u{fffd}'));
                        }
                        _ => return Err("invalid escape".into()),
                    }
                }
                c if c < 0x20 => return Err("invalid character in string literal".into()),
                _ => {
                    // The rest of a UTF-8 sequence comes as it is.
                    let start = self.at - 1;
                    let mut end = self.at;
                    while end < self.s.len() && (self.s[end] & 0xc0) == 0x80 {
                        end += 1;
                    }
                    out.push_str(&String::from_utf8_lossy(&self.s[start..end]));
                    self.at = end;
                }
            }
        }
    }
    fn hex4(&mut self) -> Result<u32, String> {
        let h = self.s.get(self.at..self.at + 4).ok_or("unexpected end of JSON input")?;
        self.at += 4;
        u32::from_str_radix(std::str::from_utf8(h).map_err(|_| "invalid escape")?, 16).map_err(|_| "invalid escape".into())
    }
    // A number's text, validated against JSON's grammar.
    fn number(&mut self) -> Result<&'a str, String> {
        self.ws();
        let start = self.at;
        let d = |r: &Self, i: usize| r.s.get(i).map_or(false, |c| c.is_ascii_digit());
        if self.s.get(self.at) == Some(&b'-') {
            self.at += 1;
        }
        if self.s.get(self.at) == Some(&b'0') {
            self.at += 1;
        } else if d(self, self.at) {
            while d(self, self.at) {
                self.at += 1;
            }
        } else {
            return Err("invalid number".into());
        }
        if self.s.get(self.at) == Some(&b'.') {
            self.at += 1;
            if !d(self, self.at) {
                return Err("invalid number".into());
            }
            while d(self, self.at) {
                self.at += 1;
            }
        }
        if matches!(self.s.get(self.at), Some(b'e') | Some(b'E')) {
            self.at += 1;
            if matches!(self.s.get(self.at), Some(b'+') | Some(b'-')) {
                self.at += 1;
            }
            if !d(self, self.at) {
                return Err("invalid number".into());
            }
            while d(self, self.at) {
                self.at += 1;
            }
        }
        Ok(std::str::from_utf8(&self.s[start..self.at]).unwrap())
    }
    // Any value, read past; its kind for a type mismatch.
    fn skip(&mut self) -> Result<&'static str, String> {
        match self.peek().ok_or("unexpected end of JSON input")? {
            b'{' => {
                self.at += 1;
                if self.peek() == Some(b'}') {
                    self.at += 1;
                    return Ok("object");
                }
                loop {
                    self.string()?;
                    self.eat(b':')?;
                    self.skip()?;
                    match self.peek() {
                        Some(b',') => self.at += 1,
                        Some(b'}') => {
                            self.at += 1;
                            return Ok("object");
                        }
                        _ => return Err("expected ',' or '}'".into()),
                    }
                }
            }
            b'[' => {
                self.at += 1;
                if self.peek() == Some(b']') {
                    self.at += 1;
                    return Ok("array");
                }
                loop {
                    self.skip()?;
                    match self.peek() {
                        Some(b',') => self.at += 1,
                        Some(b']') => {
                            self.at += 1;
                            return Ok("array");
                        }
                        _ => return Err("expected ',' or ']'".into()),
                    }
                }
            }
            b'"' => self.string().map(|_| "string"),
            b't' => self.word(b"true").map(|_| "bool"),
            b'f' => self.word(b"false").map(|_| "bool"),
            b'n' => self.word(b"null").map(|_| "null"),
            _ => self.number().map(|_| "number"),
        }
    }
}

fn read_order(body: &[u8]) -> Result<OrderIn, String> {
    let mut r = Reader { s: body, at: 0 };
    let mut o = OrderIn::default();
    if r.peek() != Some(b'{') {
        r.skip()?;
        r.ws();
        if r.at != body.len() {
            return Err("invalid character after top-level value".into());
        }
        o.mismatch = Some(("", "main.OrderIn"));
        return Ok(o);
    }
    r.at += 1;
    if r.peek() == Some(b'}') {
        r.at += 1;
    } else {
        loop {
            let key = r.string()?;
            r.eat(b':')?;
            match key.as_str() {
                "item_id" | "quantity" => {
                    let field: &'static str = if key == "item_id" { "item_id" } else { "quantity" };
                    let v = match r.peek() {
                        Some(b'n') => {
                            r.word(b"null")?;
                            None
                        }
                        Some(c) if c == b'-' || c.is_ascii_digit() => {
                            let text = r.number()?;
                            match text.parse::<i64>() {
                                Ok(n) => Some(n),
                                Err(_) => {
                                    o.mismatch.get_or_insert((field, "int64"));
                                    None
                                }
                            }
                        }
                        _ => {
                            r.skip()?;
                            o.mismatch.get_or_insert((field, "int64"));
                            None
                        }
                    };
                    if field == "item_id" {
                        o.item_id = v;
                    } else {
                        o.quantity = v;
                    }
                }
                "note" => match r.peek() {
                    Some(b'n') => {
                        r.word(b"null")?;
                        o.note = None;
                    }
                    Some(b'"') => o.note = Some(r.string()?),
                    _ => {
                        r.skip()?;
                        o.mismatch.get_or_insert(("note", "string"));
                    }
                },
                _ => {
                    r.skip()?;
                }
            }
            match r.peek() {
                Some(b',') => r.at += 1,
                Some(b'}') => {
                    r.at += 1;
                    break;
                }
                _ => return Err("expected ',' or '}' after object key:value pair".into()),
            }
        }
    }
    r.ws();
    if r.at != body.len() {
        return Err("invalid character after top-level value".into());
    }
    Ok(o)
}

fn problem(out: &mut Vec<u8>, loc: &[&str], msg: &str, kind: &str) {
    if out.len() > 1 {
        out.push(b',');
    }
    out.extend_from_slice(b"{\"loc\":[");
    for (i, l) in loc.iter().enumerate() {
        if i > 0 {
            out.push(b',');
        }
        json_str(out, l);
    }
    out.extend_from_slice(b"],\"msg\":");
    json_str(out, msg);
    out.extend_from_slice(b",\"type\":");
    json_str(out, kind);
    out.push(b'}');
}

fn order(body: &[u8]) -> (u16, Vec<u8>) {
    let mut ps = b"[".to_vec();
    match read_order(body) {
        Err(e) => problem(&mut ps, &["body"], &format!("JSON decode error: {e}"), "json_invalid"),
        Ok(o) => {
            if let Some((field, ty)) = o.mismatch {
                let loc: Vec<&str> = if field.is_empty() { vec!["body"] } else { vec!["body", field] };
                problem(&mut ps, &loc, &format!("expected {ty}"), "value_error");
            } else {
                match o.item_id {
                    None => problem(&mut ps, &["body", "item_id"], "Field required", "missing"),
                    Some(id) if id < 1 || id > CATALOG.len() as i64 => {
                        problem(&mut ps, &["body", "item_id"], "No item has this id", "value_error")
                    }
                    _ => {}
                }
                match o.quantity {
                    None => problem(&mut ps, &["body", "quantity"], "Field required", "missing"),
                    Some(q) if q < 1 => problem(
                        &mut ps,
                        &["body", "quantity"],
                        "Input should be greater than or equal to 1",
                        "greater_than_equal",
                    ),
                    Some(q) if q > 100 => {
                        problem(&mut ps, &["body", "quantity"], "Input should be less than or equal to 100", "less_than_equal")
                    }
                    _ => {}
                }
                if o.note.as_ref().map_or(false, |n| n.len() > 64) {
                    problem(&mut ps, &["body", "note"], "a string longer than its field", "value_error");
                }
                if ps.len() == 1 {
                    let it = &CATALOG[(o.item_id.unwrap() - 1) as usize];
                    if !it.in_stock {
                        return detail(409, "Out of stock");
                    }
                    let q = o.quantity.unwrap();
                    let mut b = format!(
                        "{{\"id\":{},\"item_id\":{},\"quantity\":{},\"total_cents\":{},\"note\":",
                        NEXT_ORDER.fetch_add(1, Ordering::Relaxed) + 1,
                        it.id,
                        q,
                        it.price_cents * q
                    )
                    .into_bytes();
                    json_str(&mut b, o.note.as_deref().unwrap_or(""));
                    b.push(b'}');
                    return (201, b);
                }
            }
        }
    }
    ps.push(b']');
    let mut b = b"{\"detail\":".to_vec();
    b.extend_from_slice(&ps);
    b.push(b'}');
    (422, b)
}

// ---------------------------------------------------------------- routes

struct Response {
    status: u16,
    allow: &'static str,
    body: Vec<u8>,
}

fn handle(method: &[u8], path: &[u8], body: &[u8]) -> Response {
    let not_allowed = |allow: &'static str| {
        let (status, body) = detail(405, "Method Not Allowed");
        Response { status, allow, body }
    };
    let ok = |(status, body): (u16, Vec<u8>)| Response { status, allow: "", body };
    if path == b"/health" {
        if method != b"GET" {
            return not_allowed("GET");
        }
        return ok((200, b"{\"status\":\"ok\"}".to_vec()));
    }
    if path == b"/v1/items" {
        if method != b"GET" {
            return not_allowed("GET");
        }
        let mut b = b"{\"items\":[".to_vec();
        for (i, it) in CATALOG.iter().enumerate() {
            if i > 0 {
                b.push(b',');
            }
            json_item(&mut b, it);
        }
        b.extend_from_slice(b"]}");
        return ok((200, b));
    }
    if path.len() > 10 && path.starts_with(b"/v1/items/") {
        if method != b"GET" {
            return not_allowed("GET");
        }
        let id = std::str::from_utf8(&path[10..]).ok().and_then(|s| s.parse::<i64>().ok());
        return match id {
            Some(id) if id >= 1 && id <= CATALOG.len() as i64 => {
                let mut b = Vec::new();
                json_item(&mut b, &CATALOG[(id - 1) as usize]);
                ok((200, b))
            }
            _ => ok(detail(404, "Not Found")),
        };
    }
    if path == b"/v1/orders" {
        if method != b"POST" {
            return not_allowed("POST");
        }
        return ok(order(body));
    }
    ok(detail(404, "Not Found"))
}

// ---------------------------------------------------------------- HTTP

fn equal_fold(s: &[u8], word: &str) -> bool {
    s.len() == word.len() && s.eq_ignore_ascii_case(word.as_bytes())
}

fn trim(mut s: &[u8]) -> &[u8] {
    while let [b' ' | b'\t', rest @ ..] = s {
        s = rest;
    }
    while let [rest @ .., b' ' | b'\t'] = s {
        s = rest;
    }
    s
}

fn has_token(v: &[u8], tok: &str) -> bool {
    v.split(|&c| c == b',').any(|p| equal_fold(trim(p), tok))
}

struct Head {
    method: (usize, usize),
    path: (usize, usize),
    keep_alive: bool,
    length: usize,
}

fn find(h: &[u8], from: usize, c: u8) -> Option<usize> {
    h[from..].iter().position(|&x| x == c).map(|i| i + from)
}

// The request's head as stdlib/http.goose's parse_head reads it, or the status to
// refuse it with.
fn parse_head(head: &[u8]) -> Result<Head, u16> {
    let eol = find(head, 0, b'\r').ok_or(400u16)?;
    let line = &head[..eol];
    let sp1 = find(line, 0, b' ').filter(|&i| i > 0).ok_or(400u16)?;
    let sp2 = find(line, sp1 + 1, b' ').filter(|&i| i > sp1 + 1).ok_or(400u16)?;
    let target = &line[sp1 + 1..sp2];
    let version = &line[sp2 + 1..];
    if target[0] != b'/' {
        return Err(400);
    }
    let path_end = find(target, 0, b'?').unwrap_or(target.len());
    let mut keep_alive = match version {
        b"HTTP/1.1" => true,
        b"HTTP/1.0" => false,
        _ => return Err(400),
    };
    let (mut length, mut seen, mut hosts) = (0usize, false, 0);
    let mut at = eol + 2;
    while at + 2 < head.len() {
        let end = find(head, at, b'\r').ok_or(400u16)?;
        let h = &head[at..end];
        at = end + 2;
        let colon = find(h, 0, b':').filter(|&i| i > 0).ok_or(400u16)?;
        let (name, value) = (&h[..colon], trim(&h[colon + 1..]));
        if equal_fold(name, "content-length") {
            if value.is_empty() {
                return Err(400);
            }
            let mut n = 0usize;
            for &c in value {
                if !c.is_ascii_digit() {
                    return Err(400);
                }
                n = (n * 10 + (c - b'0') as usize).min(MAX_BODY + 1);
            }
            if seen && n != length {
                return Err(400);
            }
            length = n;
            seen = true;
        } else if equal_fold(name, "transfer-encoding") {
            return Err(501);
        } else if equal_fold(name, "connection") {
            if has_token(value, "close") {
                keep_alive = false;
            } else if has_token(value, "keep-alive") {
                keep_alive = true;
            }
        } else if equal_fold(name, "host") {
            hosts += 1;
        }
    }
    if hosts > 1 || (hosts == 0 && version == b"HTTP/1.1") {
        return Err(400);
    }
    if length > MAX_BODY {
        return Err(413);
    }
    Ok(Head { method: (0, sp1), path: (sp1 + 1, sp1 + 1 + path_end), keep_alive, length })
}

async fn send(c: &mut TcpStream, out: &mut Vec<u8>, res: &Response, keep: bool) -> bool {
    out.clear();
    out.extend_from_slice(
        format!(
            "HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\n",
            res.status,
            reason(res.status),
            res.body.len()
        )
        .as_bytes(),
    );
    if !res.allow.is_empty() {
        out.extend_from_slice(format!("Allow: {}\r\n", res.allow).as_bytes());
    }
    if !keep {
        out.extend_from_slice(b"Connection: close\r\n");
    }
    out.extend_from_slice(b"\r\n");
    out.extend_from_slice(&res.body);
    c.write_all(out).await.is_ok()
}

// More bytes into buf[have..], or 0 when the connection should end: closed,
// failed, idle, or stopping with nothing read.
async fn fill(c: &mut TcpStream, buf: &mut [u8], have: usize) -> usize {
    let deadline = Instant::now() + IDLE;
    loop {
        if have == 0 && STOPPING.load(Ordering::Relaxed) {
            return 0;
        }
        match tokio::time::timeout(POLL, c.read(&mut buf[have..])).await {
            Ok(Ok(n)) => return n,
            Ok(Err(_)) => return 0,
            Err(_) if Instant::now() < deadline => continue,
            Err(_) => return 0,
        }
    }
}

async fn refuse(c: &mut TcpStream, out: &mut Vec<u8>, status: u16) {
    let res = Response { status, allow: "", body: format!("{{\"detail\":\"{}\"}}", reason(status)).into_bytes() };
    send(c, out, &res, false).await;
}

async fn serve(mut c: TcpStream) {
    let _ = c.set_nodelay(true);
    let mut buf = vec![0u8; MAX_HEAD + MAX_BODY];
    let mut out = Vec::with_capacity(1024);
    let mut have = 0;
    loop {
        let end;
        loop {
            if let Some(i) = buf[..have].windows(4).position(|w| w == b"\r\n\r\n") {
                end = i + 4;
                break;
            }
            if have >= MAX_HEAD {
                return refuse(&mut c, &mut out, 431).await;
            }
            let n = fill(&mut c, &mut buf, have).await;
            if n == 0 {
                return;
            }
            have += n;
        }
        if end > MAX_HEAD {
            return refuse(&mut c, &mut out, 431).await;
        }
        let head = match parse_head(&buf[..end]) {
            Ok(h) => h,
            Err(status) => return refuse(&mut c, &mut out, status).await,
        };
        while have < end + head.length {
            let n = fill(&mut c, &mut buf, have).await;
            if n == 0 {
                return;
            }
            have += n;
        }
        let done = end + head.length;
        let res = handle(&buf[head.method.0..head.method.1], &buf[head.path.0..head.path.1], &buf[end..done]);
        let keep = head.keep_alive && !STOPPING.load(Ordering::Relaxed);
        if !send(&mut c, &mut out, &res, keep).await || !keep {
            return;
        }
        buf.copy_within(done..have, 0);
        have -= done;
    }
}

// Ctrl-C, Ctrl-Break (Windows) or SIGTERM: stopping set, and stop woken.
fn on_stop(stop: Arc<tokio::sync::Notify>) {
    let wake = move || {
        STOPPING.store(true, Ordering::Relaxed);
        stop.notify_one();
    };
    let w = wake.clone();
    tokio::spawn(async move {
        if tokio::signal::ctrl_c().await.is_ok() {
            w();
        }
    });
    #[cfg(windows)]
    tokio::spawn(async move {
        if let Ok(mut brk) = tokio::signal::windows::ctrl_break() {
            if brk.recv().await.is_some() {
                wake();
            }
        }
    });
    #[cfg(unix)]
    tokio::spawn(async move {
        use tokio::signal::unix::{signal, SignalKind};
        if let Ok(mut term) = signal(SignalKind::terminate()) {
            if term.recv().await.is_some() {
                wake();
            }
        }
    });
}

fn main() {
    let port = std::env::args().nth(1).unwrap_or_else(|| "8080".into());
    // As many workers as the Go twins get GOMAXPROCS: TOKIO_WORKER_THREADS,
    // which tokio reads itself, or else one per hardware thread.
    let rt = tokio::runtime::Builder::new_multi_thread().enable_all().build().unwrap();
    rt.block_on(async move {
        let ln = match TcpListener::bind(format!("0.0.0.0:{port}")).await {
            Ok(l) => l,
            Err(e) => {
                eprintln!("{e}");
                std::process::exit(1);
            }
        };
        println!("listening on :{port}");
        let served = Arc::new(AtomicU64::new(0));
        let live = Arc::new(AtomicI64::new(0));
        let stop = Arc::new(tokio::sync::Notify::new());
        on_stop(stop.clone());
        // The acceptor, a task of its own that the stop ends, the listener
        // with it.
        let (s, l) = (served.clone(), live.clone());
        let acceptor = tokio::spawn(async move {
            loop {
                if let Ok((c, _)) = ln.accept().await {
                    s.fetch_add(1, Ordering::Relaxed);
                    l.fetch_add(1, Ordering::Relaxed);
                    let l = l.clone();
                    tokio::spawn(async move {
                        serve(c).await;
                        l.fetch_sub(1, Ordering::Relaxed);
                    });
                }
            }
        });
        stop.notified().await;
        acceptor.abort();
        let _ = acceptor.await;
        println!("stopping");
        let deadline = Instant::now() + Duration::from_secs(10);
        while live.load(Ordering::Relaxed) > 0 && Instant::now() < deadline {
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
        println!("served {} connection(s)", served.load(Ordering::Relaxed));
    });
}
