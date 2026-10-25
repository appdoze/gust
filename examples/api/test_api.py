"""Checks the API example, Gust's or Go's, over real sockets.

    python examples/api/test_api.py --exe path/to/api.exe [--crash-route]

Starts the server on a free port, sends each case, compares the status and
the JSON body (and a few headers), then stops it with Ctrl-C's signal and
checks the graceful shutdown. Exits 1 on any mismatch.
"""
import argparse, json, os, signal, socket, subprocess, sys, time

def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]

def read_response(f):
    """One HTTP/1.1 response from a socket file: status, headers, body."""
    line = f.readline()
    if not line:
        return None
    parts = line.decode().split(" ", 2)
    status = int(parts[1])
    headers = {}
    while True:
        h = f.readline()
        if h in (b"\r\n", b""):
            break
        k, v = h.decode().split(":", 1)
        headers[k.strip().lower()] = v.strip()
    body = f.read(int(headers.get("content-length", "0")))
    return status, headers, body

def request(port, raw, keep=None):
    """Sends raw bytes on a fresh connection; the response(s) to it."""
    with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
        s.sendall(raw)
        f = s.makefile("rb")
        return read_response(f)

def http(method, path, body=None, headers=""):
    b = b"" if body is None else (body if isinstance(body, bytes) else json.dumps(body).encode())
    head = f"{method} {path} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n{headers}"
    if body is not None:
        head += f"Content-Type: application/json\r\nContent-Length: {len(b)}\r\n"
    return head.encode() + b"\r\n" + b

ITEM1 = {"id": 1, "name": "Espresso", "price_cents": 250, "rating": 4.6, "in_stock": True}

def problems(*locs):
    return [list(l) for l in locs]

CASES = [
    # name, request, status, check(body json) -> bool
    ("health", http("GET", "/health"), 200, lambda j: j == {"status": "ok"}),
    ("list", http("GET", "/v1/items"), 200, lambda j: len(j["items"]) == 8 and j["items"][0] == ITEM1),
    ("item", http("GET", "/v1/items/1"), 200, lambda j: j == ITEM1),
    ("item query ignored", http("GET", "/v1/items/3?x=1"), 200, lambda j: j["name"] == "Flat white"),
    ("item missing", http("GET", "/v1/items/99"), 404, lambda j: j == {"detail": "Not Found"}),
    ("item bad id", http("GET", "/v1/items/abc"), 404, lambda j: j == {"detail": "Not Found"}),
    ("no route", http("GET", "/nope"), 404, lambda j: j == {"detail": "Not Found"}),
    ("wrong method", http("DELETE", "/v1/items/1"), 405, lambda j: j == {"detail": "Method Not Allowed"}),
    ("order", http("POST", "/v1/orders", {"item_id": 2, "quantity": 3, "note": "oat"}), 201,
     lambda j: j["item_id"] == 2 and j["quantity"] == 3 and j["total_cents"] == 1140 and j["note"] == "oat" and j["id"] > 0),
    ("order unknown member skipped", http("POST", "/v1/orders", {"item_id": 1, "quantity": 1, "extra": [1, {"a": None}]}), 201,
     lambda j: j["total_cents"] == 250 and j["note"] == ""),
    ("order missing fields", http("POST", "/v1/orders", {"note": "x"}), 422,
     lambda j: [p["loc"] for p in j["detail"]] == [["body", "item_id"], ["body", "quantity"]]
               and all(p["type"] == "missing" for p in j["detail"])),
    ("order out of range", http("POST", "/v1/orders", {"item_id": 1, "quantity": 101}), 422,
     lambda j: j["detail"][0]["loc"] == ["body", "quantity"] and j["detail"][0]["type"] == "less_than_equal"),
    ("order unknown item", http("POST", "/v1/orders", {"item_id": 42, "quantity": 1}), 422,
     lambda j: j["detail"][0]["loc"] == ["body", "item_id"]),
    ("order wrong type", http("POST", "/v1/orders", {"item_id": "one", "quantity": 1}), 422,
     lambda j: j["detail"][0]["loc"] == ["body", "item_id"] and j["detail"][0]["type"] == "value_error"),
    ("order note too long", http("POST", "/v1/orders", {"item_id": 1, "quantity": 1, "note": "n" * 65}), 422,
     lambda j: j["detail"][0]["loc"] == ["body", "note"]),
    ("order bad json", http("POST", "/v1/orders", b'{"item_id": 1,'), 422,
     lambda j: j["detail"][0]["loc"] == ["body"] and j["detail"][0]["type"] == "json_invalid"),
    ("order trailing", http("POST", "/v1/orders", b'{"item_id": 1, "quantity": 1} x'), 422,
     lambda j: j["detail"][0]["type"] == "json_invalid"),
    ("order out of stock", http("POST", "/v1/orders", {"item_id": 4, "quantity": 1}), 409,
     lambda j: j == {"detail": "Out of stock"}),
    ("order wrong method", http("GET", "/v1/orders"), 405, lambda j: True),
    ("body too large", http("POST", "/v1/orders", b"x" * 9000), 413, lambda j: True),
    # Past both servers' limits: Gust's 8 KiB, and Go's MaxHeaderBytes plus
    # the 4 KiB net/http allows over it.
    ("head too large", b"GET /health HTTP/1.1\r\nHost: x\r\nX-Big: " + b"a" * 14000 + b"\r\n\r\n", 431, lambda j: True),
    ("chunked refused", b"POST /v1/orders HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 501, lambda j: True),
    ("bad request line", b"GARBAGE\r\n\r\n", 400, lambda j: True),
    ("bad content-length", b"POST /v1/orders HTTP/1.1\r\nHost: x\r\nContent-Length: 1x\r\n\r\n", 400, lambda j: True),
    ("no host", b"GET /health HTTP/1.1\r\n\r\n", 400, lambda j: True),
    ("two hosts", b"GET /health HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400, lambda j: True),
    ("HTTP/1.0 without host", b"GET /health HTTP/1.0\r\n\r\n", 200, lambda j: j == {"status": "ok"}),
]

# Under --go: refusals net/http makes itself, before a handler runs, are
# text/plain, so only their status is compared; and net/http accepts chunked
# bodies, which Gust's server refuses.
GO_STATUS_ONLY = {"head too large", "bad request line", "bad content-length", "no host", "two hosts"}
GO_SKIP = {"chunked refused"}

def keep_alive_and_pipelining(port, fails):
    """Three requests on one connection, the last two sent together."""
    with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
        f = s.makefile("rb")
        s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
        r1 = read_response(f)
        s.sendall(b"GET /v1/items/2 HTTP/1.1\r\nHost: x\r\n\r\n"
                  b"POST /v1/orders HTTP/1.1\r\nHost: x\r\nContent-Length: 27\r\n\r\n{\"item_id\":1,\"quantity\":2}\n")
        r2 = read_response(f)
        r3 = read_response(f)
        ok = (r1 and r1[0] == 200 and r2 and r2[0] == 200 and json.loads(r2[2])["id"] == 2
              and r3 and r3[0] == 201 and json.loads(r3[2])["total_cents"] == 500
              and "connection" not in r1[1])
        # Two orders on one connection get distinct ids.
        s.sendall(b"POST /v1/orders HTTP/1.1\r\nHost: x\r\nContent-Length: 27\r\n\r\n{\"item_id\":1,\"quantity\":2}\n")
        r4 = read_response(f)
        ok = ok and r4 and r4[0] == 201 and json.loads(r4[2])["id"] != json.loads(r3[2])["id"]
    print(("ok   " if ok else "FAIL ") + "keep-alive and pipelining")
    if not ok:
        fails.append("keep-alive")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--crash-route", action="store_true",
                    help="also check that an aborting handler ends only its connection (Gust)")
    ap.add_argument("--go", action="store_true", help="the server is the Go twin")
    args = ap.parse_args()
    port = free_port()
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    cmd = [args.exe, str(port)] + (["--crash-route"] if args.crash_route else [])
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=flags)
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.05)
    fails = []
    for name, raw, want, check in CASES:
        if args.go and name in GO_SKIP:
            continue
        try:
            r = request(port, raw)
            status, headers, body = r
            if args.go and name in GO_STATUS_ONLY:
                good = status == want
            else:
                good = status == want and headers.get("content-type") == "application/json"
                if good:
                    good = check(json.loads(body)) if body else want >= 400
            if want == 405:
                good = good and headers.get("allow") in ("GET", "POST")
        except Exception as e:
            good, status, body = False, "error", repr(e).encode()
        print(("ok   " if good else "FAIL ") + f"{name}: {status} {body[:160]!r}")
        if not good:
            fails.append(name)
    keep_alive_and_pipelining(port, fails)

    if args.crash_route:
        try:
            r = request(port, http("GET", "/debug/crash"))
            crashed = r is None
        except (ConnectionError, socket.timeout):
            crashed = True
        after = request(port, http("GET", "/health"))
        ok = crashed and after and after[0] == 200
        print(("ok   " if ok else "FAIL ") + "an aborting handler closes its connection; the server keeps serving")
        if not ok:
            fails.append("crash")

    # Graceful shutdown with a connection open and idle: it is closed, and
    # the server exits on its own.
    idle = socket.create_connection(("127.0.0.1", port), timeout=5)
    time.sleep(0.2)
    if os.name == "nt":
        proc.send_signal(signal.CTRL_BREAK_EVENT)
    else:
        proc.send_signal(signal.SIGINT)
    try:
        out, err = proc.communicate(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
        fails.append("shutdown timeout")
    idle.close()
    text = out.decode(errors="replace")
    print("server stdout:", text.strip().replace("\n", " | "))
    if err.strip():
        print("server stderr:", err.decode(errors="replace").strip().replace("\n", " | "))
    ok = proc.returncode == 0 and "served" in text
    if args.crash_route:
        ok = ok and "1 aborted" in text and b"index" in err
    print(("ok   " if ok else "FAIL ") + f"graceful shutdown (exit {proc.returncode})")
    if not ok:
        fails.append("shutdown")
    print(f"{len(fails)} failure(s)" if fails else "all checks passed")
    sys.exit(1 if fails else 0)

if __name__ == "__main__":
    main()
