// Gust: the standard library's HTTP server (http::serve, stdlib/http.goose)
// on a thread of its own, answered by the same module's client on the main
// thread: a plain-text body, JSON, a 404, a POST echoed back, a body
// streamed in chunks, and a body past the server's limit, which it refuses;
// then a stop, and what the server counted.
import std;
import net;
import http;

struct App { conn: i64, left: i64 = 0 }

fn handle(a: App&, req: http::Request&, res: http::Response&, out: u8[>..]&) {
    if req.path == "/text" {
        res.content_type = "text/plain";
        out.append("plain words");
    } else if req.path == "/json" {
        out.append("{\"ok\":true}");
    } else if req.path == "/echo" && req.method == "POST" {
        res.status = 201;
        out.append(req.body);
    } else if req.path == "/stream" {
        res.stream = true;
        a.left = 5;
        out.append("[");
    } else {
        res.status = 404;
        out.append("{\"detail\":\"Not Found\"}");
    }
}

// The streamed body's next part: a number at a time, then its end.
fn more(a: App&, out: u8[>..]&, room: i64) -> bool {
    if a.left > 0 {
        format(out, a.left, if a.left > 1 { "," } else { "" });
        a.left -= 1;
        return false;
    }
    out.append("]");
    return true;
}

thread_fn server(l: net::Listener) {
    let p, e = net::poller();
    if e < 0 { return; }
    qput(http::serve(l, p, net::Conn { .. }, App { conn: 0, .. }, 64, 0, 1, handle, more));
}

fn main() {
    let l, e = net::listen(0, 16, true);
    if e < 0 {
        print("listen failed ", e);
        return;
    }
    let port = net::port(l);
    let t = thread_spawn(server, l);
    var body: u8[>..] = [];
    print("text ", http::get("127.0.0.1", port, "/text", body), " ", body);
    body.clear();
    print("json ", http::get("127.0.0.1", port, "/json", body), " ", body);
    body.clear();
    print("missing ", http::get("127.0.0.1", port, "/nope", body), " ", body);
    body.clear();
    print("post ", http::post("127.0.0.1", port, "/echo", "name=gust", "text/plain", body), " ", body);
    body.clear();
    print("stream ", http::get("127.0.0.1", port, "/stream", body), " ", body);
    body.clear();
    var big: u8[>..] = [];
    big.resize(9000, 'x');
    print("too large ", http::post("127.0.0.1", port, "/echo", big, "text/plain", body), " ", body);
    net::request_stop();
    let c = qget<http::Counts>();
    print("requests ", c.requests, " ok ", c.ok, " client errors ", c.client_errors, " refused ", c.refused,
          " connections ", c.connections);
    thread_wait(t);
    net::close(l);
}
