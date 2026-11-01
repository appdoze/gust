// What http's server takes for a request: what a proxy in front could read
// differently is refused (RFC 9112 §11.2), and what follows the rules goes on
// being accepted.
import std;
import http;

fn check(name: u8[:], head: u8[:]) {
    let req, length, status = http::parse_head(head);
    if status != 0 {
        print(name, ": refused ", status);
    } else {
        print(name, ": ", req.method, " ", req.path, " ?", req.query, " length ", length, " keep ", req.keep_alive);
    }
}

// A reply's chunk sizes, which come from whoever is on the other end.
fn check_chunks(name: u8[:], raw: u8[:]) {
    var body: u8[>..] = [];
    let taken = http::unchunk(raw, body);
    print(name, ": ", taken, " \"", body, "\"");
}

fn main() {
    check("plain", "GET /a?b=1 HTTP/1.1\r\nHost: x\r\n\r\n");
    check("http 1.0", "GET / HTTP/1.0\r\n\r\n");
    check("body", "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n");
    check("no space after colon", "POST /p HTTP/1.1\r\nHost:x\r\nContent-Length:5\r\n\r\n");
    check("spaces in value", "GET / HTTP/1.1\r\nHost: x\r\nUser-Agent: a b\tc\r\n\r\n");
    check("high bytes in value", "GET / HTTP/1.1\r\nHost: x\r\nX: \xC3\xA9\r\n\r\n");
    check("empty value", "GET / HTTP/1.1\r\nHost: x\r\nX:\r\n\r\n");
    check("close", "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    check("token method", "M-SEARCH /x HTTP/1.1\r\nHost: x\r\n\r\n");
    check("odd name", "GET / HTTP/1.1\r\nHost: x\r\nX_a.b!c: 1\r\n\r\n");

    // Smuggling, in the ways RFC 9112 warns of.
    check("space before colon", "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length : 5\r\n\r\n");
    check("tab before colon", "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length\t: 5\r\n\r\n");
    check("folded line", "POST /p HTTP/1.1\r\nHost: x\r\nX: a\r\n Content-Length: 5\r\n\r\n");
    check("bare lf in value", "POST /p HTTP/1.1\r\nHost: x\r\nX: a\nContent-Length: 5\r\n\r\n");
    check("bare cr in value", "POST /p HTTP/1.1\r\nHost: x\r\nX: a\rContent-Length: 5\r\n\r\n");
    check("bare lf line end", "GET / HTTP/1.1\nHost: x\r\n\r\n");
    check("bare cr line end", "GET / HTTP/1.1\rHost: x\r\n\r\n");
    check("nul in value", "GET / HTTP/1.1\r\nHost: x\r\nX: a\x00b\r\n\r\n");
    check("delete in value", "GET / HTTP/1.1\r\nHost: x\r\nX: a\x7Fb\r\n\r\n");
    check("two lengths", "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n");
    check("same length twice", "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n");
    check("signed length", "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: +5\r\n\r\n");
    check("length list", "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 5, 5\r\n\r\n");
    check("huge length", "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999999\r\n\r\n");
    check("chunked", "POST /p HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
    check("both", "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n");

    // Malformed in other ways.
    check("no host", "GET / HTTP/1.1\r\n\r\n");
    check("two hosts", "GET / HTTP/1.1\r\nHost: x\r\nHost: y\r\n\r\n");
    check("no colon", "GET / HTTP/1.1\r\nHost: x\r\nnonsense\r\n\r\n");
    check("no name", "GET / HTTP/1.1\r\nHost: x\r\n: value\r\n\r\n");
    check("bad version", "GET / HTTP/2.0\r\nHost: x\r\n\r\n");
    check("no version", "GET /\r\nHost: x\r\n\r\n");
    check("two spaces", "GET  / HTTP/1.1\r\nHost: x\r\n\r\n");
    check("space in target", "GET /a b HTTP/1.1\r\nHost: x\r\n\r\n");
    check("no method", " / HTTP/1.1\r\nHost: x\r\n\r\n");
    check("control in method", "G\x01T / HTTP/1.1\r\nHost: x\r\n\r\n");
    check("control in target", "GET /\x01 HTTP/1.1\r\nHost: x\r\n\r\n");
    check("relative target", "GET a HTTP/1.1\r\nHost: x\r\n\r\n");
    check("only a line end", "\r\n\r\n");

    check_chunks("two chunks", "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n");
    check_chunks("extension and trailer", "3;x=y\r\nabc\r\n0\r\nT: 1\r\n\r\n");
    check_chunks("not all here", "5\r\nabc");
    check_chunks("largest size", "7fffffffffffffff\r\nabc\r\n0\r\n\r\n");
    check_chunks("past the largest", "8000000000000000\r\nabc\r\n0\r\n\r\n");
    check_chunks("negative", "-1\r\nabc\r\n0\r\n\r\n");
    check_chunks("missing line end", "3\r\nabcXY0\r\n\r\n");
}
