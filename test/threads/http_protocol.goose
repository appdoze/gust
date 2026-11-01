// Gust: what http::serve says on the wire, over a raw connection: HEAD has the
// headers alone, a client waiting on Expect: 100-continue is told to send, an
// HTTP/1.0 client is told a connection stays open, and a request a proxy could
// read differently is refused. Then a body limit a server sets itself.
import std;
import net;
import http;

struct App { conn: i64 }

fn handle(a: App&, req: http::Request&, res: http::Response&, out: u8[>..]&) {
    if req.path == "/echo" {
        out.append(req.body);
    } else {
        out.append("hello");
    }
}

fn more(a: App&, out: u8[>..]&, room: i64) -> bool { true }

thread_fn server(l: net::Listener, max_body: i64) {
    let p, e = net::poller();
    if e < 0 { return; }
    qput(http::serve(l, p, net::Conn { .. }, App { 0 }, 64, 0, 1, max_body, handle, more));
}

// All that comes back within ms of the last of it, as text with its line ends
// shown, and whether the server closed the connection.
fn read_some(p: net::Poller, c: net::Conn, ms: i64, out: u8[>..]&) -> bool {
    var buf: u8[4096] = [0; 4096];
    loop {
        let n = net::recv(p, c, buf);
        if n == net::AGAIN {
            if http::ready(p, c, net::READ, ms) < 0 { return false; }
            continue;
        }
        if n <= 0 { return true; }
        for b in buf[..n] { out.push(b); }
    }
}

fn shown(s: const u8[:]) -> u8[>..] {
    var out: u8[>..] = [];
    for c in s {
        if c == '\r' { continue; }
        if c == '\n' { out.push('|'); } else { out.push(c); }
    }
    return out;
}

fn exchange(port: i64, name: u8[:], request: const u8[:], then_send: const u8[:] = "") {
    let p, e = net::poller();
    let c, ce = net::connect(p, "127.0.0.1", port, 5000);
    if e < 0 || ce < 0 {
        print(name, ": no connection");
        return;
    }
    var sent = 0;
    while sent < request.len {
        let n = net::send(p, c, request[sent..]);
        if n == net::AGAIN { http::ready(p, c, net::WRITE, 5000); continue; }
        if n < 0 { break; }
        sent += n;
    }
    var got: u8[>..] = [];
    var closed = read_some(p, c, 300, got);
    if then_send.len > 0 {
        print(name, " first: ", shown(got));
        got.clear();
        var more_sent = 0;
        while more_sent < then_send.len {
            let n = net::send(p, c, then_send[more_sent..]);
            if n == net::AGAIN { http::ready(p, c, net::WRITE, 5000); continue; }
            if n < 0 { break; }
            more_sent += n;
        }
        closed = read_some(p, c, 300, got);
    }
    print(name, ": ", shown(got), " closed ", closed);
    net::close(p);
}

fn main() {
    let l, e = net::listen(0, 16, true);
    if e < 0 {
        print("listen failed ", e);
        return;
    }
    let port = net::port(l);
    let t = thread_spawn(server, l, 100000);

    exchange(port, "get", "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    exchange(port, "head", "HEAD / HTTP/1.1\r\nHost: x\r\n\r\n");
    // What comes after a HEAD's reply on the connection is the next reply,
    // with nothing of a body between them.
    exchange(port, "head then get", "HEAD / HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n");
    exchange(port, "http 1.0", "GET / HTTP/1.0\r\n\r\n");
    exchange(port, "http 1.0 keep-alive", "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
    exchange(port, "close", "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

    exchange(port, "expect", "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n", "abcde");
    exchange(port, "expect with the body", "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\nabcde");
    exchange(port, "expect in 1.0", "POST /echo HTTP/1.0\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n", "abcde");
    exchange(port, "expect what", "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: tea\r\n\r\n");

    // Smuggling: the second request here must not be read out of the first's body.
    exchange(port, "space before colon", "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length : 27\r\n\r\nGET /smuggled HTTP/1.1\r\n\r\n\r\n");

    // A body past MAX_BODY, as this server allows, and past its own limit.
    var big: u8[>..] = [];
    big.resize(50000, 'x');
    var body: u8[>..] = [];
    print("large ", http::post("127.0.0.1", port, "/echo", big, "text/plain", body), " ", body.len);
    big.resize(100001, 'x');
    body.clear();
    // Refused with a 413, or the connection closed under the client still sending.
    let too_large = http::post("127.0.0.1", port, "/echo", big, "text/plain", body);
    print("too large ", too_large == 413 || too_large < 0);

    body.clear();
    print("client head ", http::request("HEAD", "127.0.0.1", port, "/", "", "", body), " ", body.len);

    net::request_stop();
    qget<http::Counts>();
    thread_wait(t);
    net::close(l);
}
