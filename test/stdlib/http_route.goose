// Gust: http::route, routing resolved at compile time (docs/stdlib.md):
// one variant per endpoint, patterns with {name} parameters going into the
// variant's fields, 405 naming the methods a path has, 404 for the rest.
// Requests are built here, so no socket is opened.
import http;

struct Session { conn: i64, orders: i64 = 0 }

enum Api {
    Health {},
    Export {},
    Item { id: i64 },
    Named { id: i64, name: u8[..64] },
    Order {},
    Cancel {},
}
fn endpoint(r: Api.Health) -> const u8[:] { "GET /health" }
fn endpoint(r: Api.Export) -> const u8[:] { "GET /v1/items/export" }
fn endpoint(r: Api.Item) -> const u8[:] { "GET /v1/items/{id}" }
fn endpoint(r: Api.Named) -> const u8[:] { "GET /v1/items/{id}/{name}" }
fn endpoint(r: Api.Order) -> const u8[:] { "POST /v1/orders" }
fn endpoint(r: Api.Cancel) -> const u8[:] { "DELETE /v1/orders" }

fn handle(r: Api.Health, s: Session&, req: http::Request&, res: http::Response&, out: u8[>..]&) {
    out.append("ok");
}
fn handle(r: Api.Export, s: Session&, req: http::Request&, res: http::Response&, out: u8[>..]&) {
    out.append("export");
}
fn handle(r: Api.Item, s: Session&, req: http::Request&, res: http::Response&, out: u8[>..]&) {
    format(out, "item ", r.id);
}
fn handle(r: Api.Named, s: Session&, req: http::Request&, res: http::Response&, out: u8[>..]&) {
    format(out, "item ", r.id, " named ", r.name);
}
fn handle(r: Api.Order, s: Session&, req: http::Request&, res: http::Response&, out: u8[>..]&) {
    s.orders += 1;
    res.status = 201;
    format(out, "order ", s.orders);
}
fn handle(r: Api.Cancel, s: Session&, req: http::Request&, res: http::Response&, out: u8[>..]&) {
    out.append("cancelled");
}

fn ask(s: Session&, method: const u8[:], path: const u8[:]) {
    let req = http::Request { method: method, path: path, query: "", body: "", keep_alive: true, http11: true };
    var res = http::Response { .. };
    var out: u8[>..] = [];
    let routed = http::route<Api, Session>(s, req, res, out);
    print(method, " ", path, ": ", routed, " ", res.status, " allow [", res.allow, "] ", out);
}

fn main() {
    var s = Session { conn: 0 };
    ask(s, "GET", "/health");                // 0 200 allow [] ok
    ask(s, "GET", "/v1/items/export");       // a literal pattern before {id}
    ask(s, "GET", "/v1/items/42");           // 0 200 allow [] item 42
    ask(s, "GET", "/v1/items/7/bolt");       // 0 200 allow [] item 7 named bolt
    ask(s, "POST", "/v1/orders");            // 0 201 allow [] order 1
    ask(s, "POST", "/v1/orders");            // 0 201 allow [] order 2
    ask(s, "GET", "/v1/orders");             // 405 405 allow [POST, DELETE]
    ask(s, "POST", "/health");               // 405 405 allow [GET]
    ask(s, "POST", "/v1/items/abc");         // the path matches {id}: 405, as Go's ServeMux
    ask(s, "GET", "/v1/items/abc");          // not an integer: 404
    ask(s, "GET", "/v1/items/");             // an empty segment is no {id}: 404
    ask(s, "GET", "/nowhere");               // 404 200 allow []
}
