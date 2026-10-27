// Gust: the standard library's `http` (docs/stdlib.md): an HTTP/1.1 client
// over `net`, one request per connection. A request returns the reply's
// status and appends its body to `body`: a body of Content-Length bytes,
// one sent in chunks (decoded), or one that ends where the server closes
// the connection. A result below 0 is net's error (connecting, sending,
// receiving), TIMEOUT where the reply did not come within timeout_ms of
// the last bytes received, or PROTOCOL for a reply that is not HTTP.
namespace http;

import std;
import net;

const PROTOCOL = -100;    // the reply was not HTTP/1.x

fn lower(c: u8) -> u8 { if c >= 'A' && c <= 'Z' { c + 32 } else { c } }

// Whether s equals word, ignoring ASCII case; word is lower case.
fn equal_fold(s: const u8[:], word: const u8[:]) -> bool {
    if s.len != word.len { return false; }
    for i in s.len {
        if lower(s[i]) != word[i] { return false; }
    }
    return true;
}

fn trim(s: const u8[:]) -> const u8[:] {
    var a = 0;
    var b = s.len;
    while a < b && (s[a] == ' ' || s[a] == '\t') { a += 1; }
    while b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r') { b -= 1; }
    return s[a..b];
}

// What the head says of the body: its status, its Content-Length (-1 when
// it has none), and whether it is sent in chunks; PROTOCOL as the status
// where the head is not HTTP/1.x.
fn parse_head(head: const u8[:]) -> i64, i64, bool {
    if !starts_with(head, "HTTP/1.") || head.len < 12 { return PROTOCOL, -1, false; }
    let status, ok = parse_int(head[9..12]);
    if !ok || status < 100 || status > 999 { return PROTOCOL, -1, false; }
    var length = -1;
    var chunked = false;
    var at = find(head, "\r\n");
    while at >= 0 && at + 2 < head.len {
        let rest = head[at + 2..];
        var end = find(rest, "\r\n");
        if end < 0 { end = rest.len; }
        let line = rest[..end];
        let colon = find(line, ":");
        if colon > 0 {
            let name = trim(line[..colon]);
            let value = trim(line[colon + 1..]);
            if equal_fold(name, "content-length") {
                let n, good = parse_int(value);
                if !good || n < 0 { return PROTOCOL, -1, false; }
                length = n;
            } else if equal_fold(name, "transfer-encoding") && equal_fold(value, "chunked") {
                chunked = true;
            }
        }
        if end == rest.len { break; }
        at = at + 2 + end;
    }
    return status, length, chunked;
}

// The body a chunked reply's bytes after its head make, appended to body:
// how many of the bytes it took, or -1 while it is not all there yet, or
// PROTOCOL for chunks that do not parse.
fn unchunk(raw: const u8[:], body: u8[>..]&) -> i64 {
    var at = 0;
    var parts: u8[>..] = [];
    loop {
        let eol = find(raw[at..], "\r\n");
        if eol < 0 { return -1; }
        var sizetext = raw[at..at + eol];
        let semi = find(sizetext, ";");
        if semi >= 0 { sizetext = sizetext[..semi]; }
        let size, ok = parse_int(trim(sizetext), 16);
        if !ok || size < 0 { return PROTOCOL; }
        at += eol + 2;
        if size == 0 {
            // The trailer: header lines, then a blank line.
            loop {
                let end = find(raw[at..], "\r\n");
                if end < 0 { return -1; }
                at += end + 2;
                if end == 0 { break; }
            }
            body.append(parts);
            return at;
        }
        if at + size + 2 > raw.len { return -1; }
        parts.append(raw[at..at + size]);
        if raw[at + size] != '\r' || raw[at + size + 1] != '\n' { return PROTOCOL; }
        at += size + 2;
    }
}

// Waits for c to be ready for `events`: 0, TIMEOUT, or net's error.
fn ready(p: net::Poller, c: net::Conn, events: i64, timeout_ms: i64) -> i64 {
    let w = net::want(p, c, events);
    if w < 0 { return w; }
    var handles: i64[1] = [0];
    var evs: i64[1] = [0];
    let n = net::wait(p, timeout_ms, handles, evs);
    if n < 0 { return n; }
    if n == 0 { return net::TIMEOUT; }
    return 0;
}

// The exchange on a connection made: the request out, then the reply in.
fn exchange(p: net::Poller, c: net::Conn, req: const u8[:], body: u8[>..]&, timeout_ms: i64) -> i64 {
    var sent = 0;
    while sent < req.len {
        let n = net::send(p, c, req[sent..]);
        if n == net::AGAIN {
            let r = ready(p, c, net::WRITE, timeout_ms);
            if r < 0 { return r; }
        } else if n < 0 {
            return n;
        } else {
            sent += n;
        }
    }
    var raw: u8[>..] = [];
    var buf: u8[>..] = [];
    buf.resize(16384, 0);
    var headend = -1;
    var status = 0;
    var length = -1;
    var chunked = false;
    loop {
        let n = net::recv(p, c, buf);
        if n == net::AGAIN {
            let r = ready(p, c, net::READ, timeout_ms);
            if r < 0 { return r; }
            continue;
        }
        if n < 0 { return n; }
        if n > 0 { raw.append(buf[..n]); }
        if headend < 0 {
            let at = find(raw, "\r\n\r\n");
            if at >= 0 {
                headend = at + 4;
                status, length, chunked = parse_head(raw[..at + 2]);
                if status < 0 { return status; }
                // A reply that has no body by its status.
                if (status >= 100 && status < 200) || status == 204 || status == 304 { return status; }
            }
        }
        if headend >= 0 {
            let rest = raw[headend..];
            if chunked {
                let took = unchunk(rest, body);
                if took >= 0 { return status; }
                if took == PROTOCOL { return PROTOCOL; }
            } else if length >= 0 && rest.len >= length {
                body.append(rest[..length]);
                return status;
            }
        }
        if n == 0 {
            // The server closed: the body is what came, unless it said more.
            if headend < 0 || chunked || (length >= 0 && raw.len - headend < length) { return PROTOCOL; }
            body.append(raw[headend..]);
            return status;
        }
    }
}

// A request: method (GET, POST, ...) of path at host and port, with a
// payload of content_type where it has one (an empty payload sends none).
fn request(method: const u8[:], host: const u8[:], port: i64, path: const u8[:], payload: const u8[:],
           content_type: const u8[:], body: u8[>..]&, timeout_ms: i64 = 10000) -> i64 {
    let p, e = net::poller();
    if e < 0 { return e; }
    let c, ce = net::connect(p, host, port, timeout_ms);
    if ce < 0 {
        net::close(p);
        return ce;
    }
    var req: u8[>..] = [];
    format(req, method, " ", path, " HTTP/1.1\r\nHost: ", host, ":", port, "\r\nConnection: close\r\n");
    if payload.len > 0 || method == "POST" || method == "PUT" {
        if content_type.len > 0 { format(req, "Content-Type: ", content_type, "\r\n"); }
        format(req, "Content-Length: ", payload.len, "\r\n");
    }
    req.append("\r\n");
    req.append(payload);
    let status = exchange(p, c, req, body, timeout_ms);
    net::close(p);
    return status;
}

fn get(host: const u8[:], port: i64, path: const u8[:], body: u8[>..]&, timeout_ms: i64 = 10000) -> i64 {
    return request("GET", host, port, path, "", "", body, timeout_ms);
}

fn post(host: const u8[:], port: i64, path: const u8[:], payload: const u8[:], content_type: const u8[:],
        body: u8[>..]&, timeout_ms: i64 = 10000) -> i64 {
    return request("POST", host, port, path, payload, content_type, body, timeout_ms);
}
