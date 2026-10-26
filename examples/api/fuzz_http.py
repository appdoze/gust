"""Fuzzes the API server over real sockets with what a hostile client sends:
valid requests (every route, query strings, JSON bodies valid and not,
pipelined pairs) mutated at random, by bytes and by HTTP's own tokens, and
JSON bodies mutated with their Content-Length kept right, so that they
reach the handlers' parsing and validation. Each case is sent on a fresh
connection, sometimes in fragments, and the client then half-closes it.

    python examples/api/fuzz_http.py --exe path/to/api [--cases 20000] [--seed 1] [--threads 16]
                                     [--server-args "--workers 4"]

Fails if any response is not a well-formed HTTP/1.1 response, if a worker
aborted (an abort on input is a way to take a worker's connections down),
if the server stops answering or exits, or if its output carries an
AddressSanitizer or UndefinedBehaviorSanitizer report (an ASan/UBSan
build, as test/fuzz_untrusted.py makes on Linux and macOS).
"""
import argparse, json, os, random, signal, socket, subprocess, sys, threading, time

TOKENS = [b"\r\n", b"\r", b"\n", b" ", b":", b"?", b"&", b"=", b"/", b"%", b"%00", b"\x00", b"\xff",
          b"Content-Length: 5\r\n", b"Content-Length: 99999999999999999999\r\n", b"Content-Length: -1\r\n",
          b"Transfer-Encoding: chunked\r\n", b"Connection: close\r\n", b"Connection: keep-alive\r\n",
          b"Host: x\r\n", b"HTTP/1.0", b"HTTP/1.1", b"HTTP/2.0", b"GET ", b"POST ", b"0", b"9" * 30,
          b"{", b"}", b"[", b"]", b"\"", b",", b"\\u0000", b"1e400", b"-", b"x" * 300]

ORDERS = [
    {"item_id": 1, "quantity": 2, "note": "door"},
    {"item_id": 99, "quantity": 1},
    {"item_id": 1, "quantity": 0, "note": "x" * 70},
    {"item_id": "1", "quantity": 2.5, "extra": [1, {"a": None}]},
    [1, 2, 3],
]


def request(method, path, body=None, keep=False, version="HTTP/1.1"):
    head = f"{method} {path} {version}\r\nHost: x\r\n"
    if not keep:
        head += "Connection: close\r\n"
    b = b""
    if body is not None:
        b = body if isinstance(body, bytes) else json.dumps(body).encode()
        head += f"Content-Type: application/json\r\nContent-Length: {len(b)}\r\n"
    return head.encode() + b"\r\n" + b


def seeds():
    out = [request("GET", "/health"), request("GET", "/v1/items"), request("GET", "/v1/items?limit=3&offset=1"),
           request("GET", "/v1/items?limit=x&offset=-1"), request("GET", "/v1/items/1"),
           request("GET", "/v1/items/abc"), request("GET", "/v1/items/99999999999999999999"),
           request("GET", "/v1/items/export"), request("DELETE", "/v1/items"), request("GET", "/nope"),
           request("GET", "/health", version="HTTP/1.0"),
           request("GET", "/health", keep=True) + request("GET", "/v1/items/2")]
    out += [request("POST", "/v1/orders", o) for o in ORDERS]
    return out


def mutate(r, data):
    b = bytearray(data)
    for _ in range(r.randint(1, 4)):
        kind = r.randrange(7)
        if not b:
            b += r.choice(TOKENS)
            continue
        at = r.randrange(len(b))
        if kind == 0:
            b[at] ^= 1 << r.randrange(8)
        elif kind == 1:
            del b[r.randrange(len(b) + 1):]
        elif kind in (2, 3):
            b[at:at] = r.choice(TOKENS)
        elif kind == 4:
            del b[at:at + r.randint(1, 8)]
        elif kind == 5:
            n = r.randint(1, 8)
            src = r.randrange(len(b))
            b[at:at + n] = b[src:src + n]
        else:
            b[at] = r.randrange(256)
    return bytes(b)


def mutate_body(r):
    """A POST whose JSON body is mutated, its length kept right."""
    body = bytearray(json.dumps(r.choice(ORDERS)).encode())
    for _ in range(r.randint(1, 3)):
        at = r.randrange(len(body) + 1)
        k = r.randrange(4)
        if k == 0 and body:
            del body[min(at, len(body) - 1)]
        elif k == 1:
            body[at:at] = r.choice([b"\"", b"\\", b"{", b"[", b"]", b"}", b",", b":", b"1e999", b"-0", b"null",
                                    b"\\ud800", b"\x00", b"\xc3", b"99999999999999999999"])
        elif k == 2 and body:
            body[min(at, len(body) - 1)] = r.randrange(32, 127)
        else:
            body[at:at] = b" " * r.randint(1, 3)
    return request("POST", "/v1/orders", bytes(body))


def well_formed(data, cut=False):
    """The statuses if data is zero or more whole HTTP/1.1 responses, else
    None. With `cut` (the connection was reset, which on Windows can drop
    what was in flight) the last may be cut short, its status line whole."""
    statuses = []
    i = 0
    while i < len(data):
        end = data.find(b"\r\n\r\n", i)
        if end < 0:
            line = data[i:].split(b"\r\n", 1)
            if cut and (len(line) == 1 or line[0].startswith(b"HTTP/1.1 ")):
                return statuses
            return None
        head = data[i:end].split(b"\r\n")
        parts = head[0].split(b" ", 2)
        if len(parts) < 2 or parts[0] != b"HTTP/1.1" or not parts[1].isdigit():
            return None
        statuses.append(int(parts[1]))
        headers = {}
        for h in head[1:]:
            k, sep, v = h.partition(b":")
            if not sep:
                return None
            headers[k.strip().lower()] = v.strip()
        i = end + 4
        if headers.get(b"transfer-encoding") == b"chunked":
            while True:
                eol = data.find(b"\r\n", i)
                if eol < 0:
                    return None
                size = int(data[i:eol], 16)
                i = eol + 2 + size + 2
                if size == 0:
                    break
        elif b"content-length" in headers:
            i += int(headers[b"content-length"])
        else:
            i = len(data)
        if i > len(data):
            return statuses if cut else None
    return statuses


def one(port, r, case):
    """The bytes the server sent back, and whether the connection was reset
    or aborted on the way: a server may answer a bad request and close
    while the rest is still being sent, which then fails (on Windows as
    WSAECONNABORTED)."""
    lost = (ConnectionResetError, ConnectionAbortedError, BrokenPipeError)
    # Out of ephemeral ports (each case's client end waits in TIME_WAIT for
    # minutes, and Windows has 16,384): the machine's limit, not the
    # server's, so wait for ports to come free.
    for attempt in range(600):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=10)
            break
        except OSError as e:
            if e.errno not in (98, 99, 10048, 10055) and getattr(e, "winerror", 0) not in (10048, 10055):
                raise
            time.sleep(1)
    else:
        raise OSError("no ephemeral port came free in 10 minutes")
    with s:
        reset = False
        try:
            if r.random() < 0.3 and len(case) > 1:
                cut = r.randrange(1, len(case))
                s.sendall(case[:cut])
                time.sleep(0.001)
                s.sendall(case[cut:])
            else:
                s.sendall(case)
            s.shutdown(socket.SHUT_WR)
        except lost:
            reset = True
        except OSError:
            pass
        got = bytearray()
        while True:
            try:
                piece = s.recv(65536)
            except lost:
                reset = True
                break
            if not piece:
                break
            got += piece
    return bytes(got), reset


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--cases", type=int, default=20000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--threads", type=int, default=16)
    ap.add_argument("--server-args", default="")
    args = ap.parse_args()
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    proc = subprocess.Popen([os.path.abspath(args.exe), str(port)] + args.server_args.split(), stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, creationflags=flags)
    deadline = time.time() + 20
    while time.time() < deadline:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.05)
    base = seeds()
    statuses = {}
    bad = []
    lock = threading.Lock()
    counter = [0]

    def worker(k):
        r = random.Random(args.seed * 1000003 + k)
        while True:
            with lock:
                if counter[0] >= args.cases or len(bad) >= 5:
                    return
                counter[0] += 1
            x = r.random()
            case = mutate_body(r) if x < 0.3 else mutate(r, r.choice(base)) if x < 0.95 else r.choice(base)
            try:
                got, reset = one(port, r, case)
            except (OSError, socket.timeout) as e:
                with lock:
                    bad.append((case, f"no answer: {e!r}"))
                continue
            st = well_formed(got, reset)
            with lock:
                if st is None:
                    bad.append((case, f"malformed response {got[:200]!r}"))
                for code in st or []:
                    statuses[code] = statuses.get(code, 0) + 1

    t0 = time.time()
    threads = [threading.Thread(target=worker, args=(k,)) for k in range(args.threads)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    wall = time.time() - t0
    alive = proc.poll() is None
    try:
        health, _ = one(port, random.Random(0), request("GET", "/health"))
        healthy = (well_formed(health) or [0])[0] == 200
    except OSError:
        healthy = False
    proc.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGINT)
    try:
        out, err = proc.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
    out, err = out.decode(errors="replace"), err.decode(errors="replace")
    said = out.strip().splitlines()[-1] if out.strip() else ""
    print(f"{counter[0]} cases in {wall:.1f} s; statuses " +
          ", ".join(f"{k}: {v}" for k, v in sorted(statuses.items())))
    print("server:", said)
    fails = []
    for case, why in bad:
        fails.append(f"{why}; sent {case[:300]!r}")
    if not alive:
        fails.append(f"the server exited during the run (code {proc.returncode})")
    if not healthy:
        fails.append("the server no longer answers /health")
    if "aborted, its connections closed" in err or " 0 aborted" not in said:
        fails.append("a worker aborted on input: " + err[-800:])
    if "AddressSanitizer" in err or "runtime error:" in err or "LeakSanitizer" in err:
        fails.append("a sanitizer reported: " + err[-2000:])
    if proc.returncode != 0:
        fails.append(f"the server exited {proc.returncode}: {err[-500:]}")
    for f in fails:
        print("FAIL", f)
    if not fails:
        print("ok: every response well formed, no worker aborted, the server still serving")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
