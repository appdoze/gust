// Gust: the standard library's HTTP client (stdlib/http.goose) against a
// server thread on loopback: a body of Content-Length bytes, a chunked body
// with a chunk extension and a trailer, an HTTP/1.0 body that ends where the
// server closes, a POST answered with its own payload, a 204 with no body,
// and a reply that is not HTTP.
import std;
import net;
import http;

fn reply(path: const u8[:], payload: const u8[:], out: u8[>..]&) {
    if path == "/plain" {
        out.append("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello");
    } else if path == "/chunked" {
        out.append("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nGust\r\n6\r\n says \r\n");
        out.append("3;note=x\r\nhi!\r\n0\r\nX-Trailer: 1\r\n\r\n");
    } else if path == "/close" {
        out.append("HTTP/1.0 404 Not Found\r\n\r\nno such page");
    } else if path == "/echo" {
        format(out, "HTTP/1.1 201 Created\r\nContent-Length: ", payload.len, "\r\n\r\n");
        out.append(payload);
    } else if path == "/empty" {
        out.append("HTTP/1.1 204 No Content\r\n\r\n");
    } else {
        out.append("nonsense\r\n\r\n");
    }
}

// The request's path, and its body as its Content-Length says, once all of
// it has come: false while it has not.
fn parse_request(raw: const u8[:], path: u8[>..]&, payload: u8[>..]&) -> bool {
    let head = find(raw, "\r\n\r\n");
    if head < 0 { return false; }
    let sp1 = find(raw, " ");
    let sp2 = find(raw[sp1 + 1..], " ");
    var length = 0;
    let cl = find(raw[..head], "Content-Length: ");
    if cl >= 0 {
        let rest = raw[cl + 16..head + 2];
        let n, ok = parse_int(rest[..find(rest, "\r\n")]);
        if ok { length = n; }
    }
    if raw.len < head + 4 + length { return false; }
    path.append(raw[sp1 + 1..sp1 + 1 + sp2]);
    payload.append(raw[head + 4..head + 4 + length]);
    return true;
}

thread_fn server(l: net::Listener, count: i64) {
    let p, e = net::poller();
    if e < 0 { return; }
    net::watch(p, l);
    var handles: i64[4] = [0, 0, 0, 0];
    var events: i64[4] = [0, 0, 0, 0];
    var buf: u8[>..] = [];
    buf.resize(4096, 0);
    for k in count {
        var c = net::Conn { .. };
        loop {
            let cc, ae = net::accept(p, l);
            if ae == 0 {
                c = cc;
                break;
            }
            if ae != net::AGAIN { return; }
            net::wait(p, 5000, handles, events);
        }
        var raw: u8[>..] = [];
        var path: u8[>..] = [];
        var payload: u8[>..] = [];
        loop {
            let n = net::recv(p, c, buf);
            if n == net::AGAIN {
                net::want(p, c, net::READ);
                net::wait(p, 5000, handles, events);
                continue;
            }
            if n <= 0 { break; }
            raw.append(buf[..n]);
            if parse_request(raw, path, payload) { break; }
        }
        var out: u8[>..] = [];
        reply(path, payload, out);
        var sent = 0;
        while sent < out.len {
            let n = net::send(p, c, out[sent..]);
            if n == net::AGAIN {
                net::want(p, c, net::WRITE);
                net::wait(p, 5000, handles, events);
            } else if n < 0 {
                break;
            } else {
                sent += n;
            }
        }
        net::close(p, c);
    }
    net::close(p);
}

fn main() {
    let l, e = net::listen(0, 16, true);
    if e < 0 {
        print("listen failed ", e);
        return;
    }
    let port = net::port(l);
    let t = thread_spawn(server, l, 6);
    var body: u8[>..] = [];
    let a = http::get("127.0.0.1", port, "/plain", body);
    print("plain ", a, " ", body);
    body.clear();
    let b = http::get("127.0.0.1", port, "/chunked", body);
    print("chunked ", b, " ", body);
    body.clear();
    let c = http::get("127.0.0.1", port, "/close", body);
    print("close ", c, " ", body);
    body.clear();
    let d = http::post("127.0.0.1", port, "/echo", "name=gust&n=1", "application/x-www-form-urlencoded", body);
    print("post ", d, " ", body);
    body.clear();
    let f = http::get("127.0.0.1", port, "/empty", body);
    print("empty ", f, " [", body, "]");
    body.clear();
    let g = http::get("127.0.0.1", port, "/nonsense", body);
    print("not http ", g == http::PROTOCOL);
    thread_wait(t);
    net::close(l);
}
