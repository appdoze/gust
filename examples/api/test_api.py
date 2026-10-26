"""Checks the API example, Gust's or Go's, over real sockets.

    python examples/api/test_api.py --exe path/to/api.exe [--crash-route]

Starts the server on a free port, sends each case, compares the status and
the JSON body (and a few headers), then stops it with Ctrl-C's signal and
checks the graceful shutdown. Exits 1 on any mismatch.
"""
import argparse, io, json, os, signal, socket, struct, subprocess, sys, threading, time

def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]

def read_response(f):
    """One HTTP/1.1 response from a socket file: status, headers, body. A
    chunked body is put back together; one with neither a length nor
    chunks runs to the end of the connection."""
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
    if headers.get("transfer-encoding") == "chunked":
        pieces = []
        while True:
            size = int(f.readline().strip(), 16)
            if size == 0:
                f.readline()
                break
            pieces.append(f.read(size))
            f.readline()
        body = b"".join(pieces)
    elif "content-length" in headers:
        body = f.read(int(headers["content-length"]))
    else:
        body = f.read()
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

def report(fails, name, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + name + (f": {detail}" if detail else ""))
    if not ok:
        fails.append(name)

def connect(port):
    # Blocking, with no timeout: on Windows a socket with a timeout waits in
    # select() on the 15.6 ms system timer.
    return socket.create_connection(("127.0.0.1", port))

def slow_reader(port, fails):
    """A client that pipelines many requests and reads nothing for a while:
    the server holds its output back (a partial write, then waiting for the
    socket) without stalling other clients, then delivers every response,
    complete and in order, once it reads."""
    n = 3000
    a = connect(port)
    a.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    reqs = b"".join(b"GET /v1/items/%d HTTP/1.1\r\nHost: x\r\n\r\n" % (1 + i % 8) for i in range(n))
    sender = threading.Thread(target=a.sendall, args=(reqs,), daemon=True)
    sender.start()
    time.sleep(0.5)
    worst, others = 0.0, True
    for _ in range(20):
        t0 = time.perf_counter()
        with connect(port) as b:
            b.sendall(http("GET", "/health"))
            r = read_response(b.makefile("rb"))
        worst = max(worst, time.perf_counter() - t0)
        others = others and r is not None and r[0] == 200
    f = a.makefile("rb")
    got = 0
    for i in range(n):
        r = read_response(f)
        if r is None or r[0] != 200 or json.loads(r[2])["id"] != 1 + i % 8:
            break
        got += 1
    sender.join(10)
    a.close()
    report(fails, "a client that does not read holds back only its own responses",
           others and worst < 1.0 and got == n,
           f"others served {others}, slowest {worst * 1000:.0f} ms; {got} of {n} responses in order")

def slow_request(port, fails):
    """A client that trickles its request is cut off at the request
    deadline (5 s from its first byte), answered 408 or closed."""
    s = connect(port)
    msg = b"GET /health HTTP/1.1\r\nHost: x\r\nX-Pad: " + b"a" * 60 + b"\r\n\r\n"
    t0 = time.time()
    answer = None
    for byte in msg:
        try:
            s.send(bytes([byte]))
        except OSError:
            answer = b"(closed)"
            break
        time.sleep(0.2)
        s.setblocking(False)
        try:
            data = s.recv(4096)
            answer = data or b"(closed)"
        except (BlockingIOError, InterruptedError):
            pass
        except OSError:
            answer = b"(closed)"
        s.setblocking(True)
        if answer is not None or time.time() - t0 > 9:
            break
    elapsed = time.time() - t0
    s.close()
    ok = answer is not None and (answer.startswith(b"HTTP/1.1 408") or answer == b"(closed)") and 4.0 < elapsed < 7.5
    report(fails, "a request trickled in is cut off at its deadline", ok,
           f"{(answer or b'no answer')[:24]!r} after {elapsed:.1f} s")

def disconnect_mid_response(port, fails):
    """A client that asks for a lot and resets the connection before reading
    it: the server drops the connection and goes on serving."""
    s = connect(port)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.sendall(b"GET /v1/items HTTP/1.1\r\nHost: x\r\n\r\n" * 300)
    time.sleep(0.1)
    s.close()
    time.sleep(0.3)
    r = request(port, http("GET", "/health"))
    report(fails, "a client resetting mid-response does not disturb the server", r is not None and r[0] == 200)

def items_in_order(body, n):
    items = json.loads(body)["items"]
    return len(items) == n and all(it["id"] == k + 1 for k, it in enumerate(items))

def large_responses(port, fails):
    """A response far past what one write sends, built whole (about 630 KB),
    and one streamed (about 13 MB, chunked), each followed on the same
    connection by a request answered after it; HTTP/1.0 streamed to the end
    of the connection; and the query's limits."""
    with connect(port) as s:
        f = s.makefile("rb")
        s.sendall(b"GET /v1/items?repeat=1000 HTTP/1.1\r\nHost: x\r\n\r\nGET /health HTTP/1.1\r\nHost: x\r\n\r\n")
        r1, r2 = read_response(f), read_response(f)
        report(fails, "a response built whole past 16 KB, then the next on its connection",
               r1 and r1[0] == 200 and items_in_order(r1[2], 8000) and r2 and r2[0] == 200,
               f"{len(r1[2]) if r1 else 0} bytes")
        s.sendall(b"GET /v1/items/export?repeat=20000 HTTP/1.1\r\nHost: x\r\n\r\n"
                  b"GET /v1/items/3 HTTP/1.1\r\nHost: x\r\n\r\n")
        r3, r4 = read_response(f), read_response(f)
        report(fails, "a streamed response (chunked), then the next on its connection",
               r3 and r3[0] == 200 and r3[1].get("transfer-encoding") == "chunked" and
               items_in_order(r3[2], 160000) and r4 and r4[0] == 200 and json.loads(r4[2])["id"] == 3,
               f"{len(r3[2]) if r3 else 0} bytes")
    with connect(port) as s:
        s.sendall(b"GET /v1/items/export?repeat=100 HTTP/1.0\r\n\r\n")
        r = read_response(s.makefile("rb"))
        report(fails, "a streamed response to HTTP/1.0 ends with its connection",
               r and r[0] == 200 and "transfer-encoding" not in r[1] and r[1].get("connection") == "close"
               and items_in_order(r[2], 800))
    ok = True
    for path in ("/v1/items?repeat=0", "/v1/items?repeat=1001", "/v1/items?repeat=x",
                 "/v1/items/export?repeat=1000001"):
        r = request(port, http("GET", path))
        ok = ok and r[0] == 422 and json.loads(r[2])["detail"][0]["loc"] == ["query", "repeat"]
    report(fails, "repeat out of range is a 422 on the query", ok)

def slow_stream_reader(port, fails):
    """A client that takes a 13 MB stream slowly: it gets every byte, and
    other clients are served promptly meanwhile."""
    a = connect(port)
    a.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
    a.sendall(b"GET /v1/items/export?repeat=20000 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
    raw = []
    def take():
        while True:
            d = a.recv(65536)
            if not d:
                return
            raw.append(d)
            time.sleep(0.004)
    reader = threading.Thread(target=take, daemon=True)
    reader.start()
    time.sleep(0.2)
    worst, others = 0.0, True
    for _ in range(20):
        t0 = time.perf_counter()
        with connect(port) as b:
            b.sendall(http("GET", "/health"))
            r = read_response(b.makefile("rb"))
        worst = max(worst, time.perf_counter() - t0)
        others = others and r is not None and r[0] == 200
    reader.join(60)
    a.close()
    r = read_response(io.BytesIO(b"".join(raw)))
    report(fails, "a slow reader of a stream gets all of it, holding back no one else",
           others and worst < 1.0 and r is not None and items_in_order(r[2], 160000),
           f"others served {others}, slowest {worst * 1000:.0f} ms; {sum(map(len, raw))} bytes")

def disconnect_mid_stream(port, fails):
    """A client that resets the connection 300 KB into a 64 MB stream: the
    server drops it and goes on serving."""
    s = connect(port)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.sendall(b"GET /v1/items/export?repeat=100000 HTTP/1.1\r\nHost: x\r\n\r\n")
    got = 0
    while got < 300000:
        d = s.recv(65536)
        if not d:
            break
        got += len(d)
    s.close()
    time.sleep(0.3)
    r = request(port, http("GET", "/health"))
    report(fails, "a client resetting mid-stream does not disturb the server", r is not None and r[0] == 200)

def fragmented_requests(port, fails):
    """A request sent three bytes at a time, and two pipelined requests split
    inside a header: each answered whole, in order."""
    with connect(port) as s:
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        f = s.makefile("rb")
        body = b'{"item_id":2,"quantity":1}'
        msg = b"POST /v1/orders HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n%s" % (len(body), body)
        for i in range(0, len(msg), 3):
            s.sendall(msg[i:i + 3])
            time.sleep(0.001)
        r1 = read_response(f)
        two = b"GET /v1/items/5 HTTP/1.1\r\nHost: x\r\n\r\nGET /v1/items/6 HTTP/1.1\r\nHo"
        s.sendall(two)
        time.sleep(0.05)
        s.sendall(b"st: x\r\n\r\n")
        r2, r3 = read_response(f), read_response(f)
    report(fails, "fragmented and split pipelined requests",
           r1 and r1[0] == 201 and r2 and json.loads(r2[2])["id"] == 5 and r3 and json.loads(r3[2])["id"] == 6)

def open_handles(pid):
    """The server's open handles (Windows) or file descriptors: sockets
    among them. None without psutil."""
    try:
        import psutil
    except ImportError:
        return None
    p = psutil.Process(pid)
    return p.num_handles() if os.name == "nt" else p.num_fds()

def churn(port, fails):
    """Connections opened, used once and closed, one after another."""
    good, t0 = 0, time.perf_counter()
    for _ in range(300):
        with connect(port) as s:
            s.sendall(http("GET", "/v1/items/2"))
            r = read_response(s.makefile("rb"))
            good += r is not None and r[0] == 200
    report(fails, "connection churn", good == 300, f"{good} of 300 in {time.perf_counter() - t0:.2f} s")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--crash-route", action="store_true",
                    help="also check that an aborting handler ends only its connection (Gust)")
    ap.add_argument("--go", action="store_true", help="the server is the Go twin")
    ap.add_argument("--server-args", default="", help="more arguments for the server, space-separated")
    args = ap.parse_args()
    port = free_port()
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    cmd = [args.exe, str(port)] + (["--crash-route"] if args.crash_route else []) + args.server_args.split()
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
    # The server's handles once it has served (Winsock, say, has started):
    # what it has again once the connections below are gone.
    handles = open_handles(proc.pid) if not args.go else None
    keep_alive_and_pipelining(port, fails)
    slow_reader(port, fails)
    disconnect_mid_response(port, fails)
    churn(port, fails)
    if not args.go:
        large_responses(port, fails)
        slow_stream_reader(port, fails)
        disconnect_mid_stream(port, fails)
        fragmented_requests(port, fails)
        slow_request(port, fails)
    if handles is not None:
        time.sleep(0.5)
        after = open_handles(proc.pid)
        report(fails, "every connection's socket is closed once it ends", after <= handles + 16,
               f"{handles} handles before the connections above, {after} after")

    if args.crash_route:
        try:
            r = request(port, http("GET", "/debug/crash"))
            crashed = r is None
        except (ConnectionError, socket.timeout):
            crashed = True
        after = request(port, http("GET", "/health"))
        report(fails, "an aborting handler closes its connection; the server keeps serving",
               crashed and after is not None and after[0] == 200)

    # Graceful shutdown with one connection idle and one halfway through a
    # request: the idle one is closed, the other is answered, with
    # Connection: close, and the server exits on its own.
    idle = connect(port)
    partial = connect(port)
    body = b'{"item_id":1,"quantity":2}'
    partial.sendall(b"POST /v1/orders HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n" % len(body))
    # And, for Gust, a stream half read: it is finished, then closed.
    streaming = None
    if not args.go:
        streaming = connect(port)
        streaming.sendall(b"GET /v1/items/export?repeat=5000 HTTP/1.1\r\nHost: x\r\n\r\n")
        first = streaming.recv(65536)
    time.sleep(0.2)
    if os.name == "nt":
        proc.send_signal(signal.CTRL_BREAK_EVENT)
    else:
        proc.send_signal(signal.SIGINT)
    time.sleep(0.5)
    partial.sendall(body)
    r = read_response(partial.makefile("rb"))
    report(fails, "a request in flight at shutdown is answered, then the connection closed",
           r is not None and r[0] == 201 and r[1].get("connection") == "close",
           f"{r[0] if r else None} {r[1].get('connection') if r else ''}")
    idle.settimeout(5)
    try:
        idle_closed = idle.recv(1) == b""
    except OSError:
        idle_closed = True
    report(fails, "an idle connection is closed at shutdown", idle_closed)
    if streaming is not None:
        streaming.settimeout(10)
        rest = [first]
        try:
            while True:
                d = streaming.recv(65536)
                if not d:
                    break
                rest.append(d)
            closed = True
        except OSError:
            closed = False
        r = read_response(io.BytesIO(b"".join(rest)))
        report(fails, "a stream under way at shutdown is finished, then its connection closed",
               closed and r is not None and items_in_order(r[2], 40000))
        streaming.close()
    try:
        out, err = proc.communicate(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
        fails.append("shutdown timeout")
    idle.close()
    partial.close()
    text = out.decode(errors="replace")
    print("server stdout:", text.strip().replace("\n", " | "))
    if err.strip():
        print("server stderr:", err.decode(errors="replace").strip().replace("\n", " | "))
    ok = proc.returncode == 0 and "served" in text
    if args.crash_route:
        ok = ok and "1 aborted" in text and b"index" in err
    report(fails, f"graceful shutdown (exit {proc.returncode})", ok)
    print(f"{len(fails)} failure(s)" if fails else "all checks passed")
    sys.exit(1 if fails else 0)

if __name__ == "__main__":
    main()