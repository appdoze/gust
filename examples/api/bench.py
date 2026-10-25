"""The API example's benchmark: Gust's server against its Go twins, under the
same load generator, on the same machine, with the same CPUs. There are two
Go servers: `go`, idiomatic net/http, and `goraw`, the same minimal HTTP/1.1
as the Gust server's (go/rawserver), so that what the language costs and
what net/http costs can be told apart.

    python examples/api/bench.py [--reps 3] [--seconds 10] [--quick] [--out results.json]

Builds the Gust server (clang, -O2), the Go server and the load generator
into build/api-bench/, then runs each scenario --reps times, alternating
the servers. The server runs on CPUs 0-7 and the load generator on 8-15
(GOMAXPROCS=8 for both Go programs), so neither takes the other's cores.

Measured per run: requests per second and latency percentiles (from the
load generator); the server's peak working set and private bytes, sampled
every 50 ms, and its peak thread count; page faults during the run; and,
for Go, heap allocations and collections from runtime.MemStats
(/debug/vars). The Gust runtime has no allocation counter: see
docs/gust/backend_api.md for how its allocations were established.
"""
import argparse, json, os, signal, socket, statistics, subprocess, sys, threading, time, urllib.request
import psutil

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(ROOT, "build", "api-bench")
EXE = ".exe" if os.name == "nt" else ""
SERVER_CPUS = list(range(0, 8))
CLIENT_CPUS = list(range(8, 16))

def build():
    os.makedirs(OUT, exist_ok=True)
    gust = os.path.join(ROOT, "build", "Release", "gust" + EXE)
    subprocess.run([gust, "build", "--cc", "clang", "-o", os.path.join(OUT, "api_gust" + EXE),
                    os.path.join(HERE, "api.gust")], check=True, capture_output=True)
    env = dict(os.environ, CGO_ENABLED="0")
    for name in ("server", "rawserver", "loadgen"):
        subprocess.run(["go", "build", "-o", os.path.join(OUT, f"{name}_go{EXE}"), f"./{name}"],
                       cwd=os.path.join(HERE, "go"), check=True, env=env)

def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]

class Server:
    def __init__(self, kind, extra=()):
        self.kind = kind
        self.port = free_port()
        exe = os.path.join(OUT, EXES[kind] + EXE)
        env = dict(os.environ, GOMAXPROCS="8")
        flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        self.proc = subprocess.Popen([exe, str(self.port), *SERVER_ARGS.get(kind, ()), *extra],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, creationflags=flags)
        self.ps = psutil.Process(self.proc.pid)
        self.ps.cpu_affinity(SERVER_CPUS)
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError(f"{kind} server did not start")

    def memory(self):
        m = self.ps.memory_info()
        return m

    def go_stats(self):
        if self.kind == "gust":
            return None
        with urllib.request.urlopen(f"http://127.0.0.1:{self.port}/debug/vars", timeout=5) as r:
            ms = json.load(r)["memstats"]
        return {k: ms[k] for k in ("Mallocs", "Frees", "TotalAlloc", "HeapInuse", "Sys", "NumGC", "PauseTotalNs")}

    def stop(self):
        self.proc.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGINT)
        try:
            out, err = self.proc.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            out, err = self.proc.communicate()
        return self.proc.returncode, out.decode(errors="replace"), err.decode(errors="replace")

class Sampler(threading.Thread):
    """The server's working set, private bytes and threads, every 50 ms."""
    def __init__(self, ps):
        super().__init__(daemon=True)
        self.ps, self.stop, self.peak_rss, self.peak_private, self.peak_threads = ps, False, 0, 0, 0
    def run(self):
        while not self.stop:
            try:
                m = self.ps.memory_info()
                self.peak_rss = max(self.peak_rss, m.rss)
                self.peak_private = max(self.peak_private, getattr(m, "private", m.vms))
                self.peak_threads = max(self.peak_threads, self.ps.num_threads())
            except psutil.Error:
                return
            time.sleep(0.05)

def cpu_seconds(ps):
    t = ps.cpu_times()
    return t.user + t.system

def loadgen(port, args, server_ps):
    """Runs the load generator; its result, with the CPU the server and the
    load generator used while it ran, each as a share of its 8 CPUs: the
    side near 100% is the bottleneck."""
    exe = os.path.join(OUT, "loadgen_go" + EXE)
    s0, t0 = cpu_seconds(server_ps), time.time()
    p = subprocess.Popen([exe, "-addr", f"127.0.0.1:{port}"] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         env=dict(os.environ, GOMAXPROCS="8"))
    lp = psutil.Process(p.pid)
    lp.cpu_affinity(CLIENT_CPUS)
    client = 0.0
    out_parts = []
    reader = threading.Thread(target=lambda: out_parts.append(p.communicate()), daemon=True)
    reader.start()
    while reader.is_alive():
        try:
            client = cpu_seconds(lp)
        except psutil.Error:
            pass
        reader.join(0.1)
    wall = time.time() - t0
    out, err = out_parts[0]
    if p.returncode != 0:
        raise RuntimeError(f"loadgen failed: {err.decode()}")
    res = json.loads(out)
    res["server_cpu_share"] = (cpu_seconds(server_ps) - s0) / wall / len(SERVER_CPUS)
    res["client_cpu_share"] = client / wall / len(CLIENT_CPUS)
    return res

def run_one(kind, scenario, seconds):
    srv = Server(kind)
    try:
        before_mem = srv.memory()
        go0 = srv.go_stats()
        faults0 = before_mem.num_page_faults if hasattr(before_mem, "num_page_faults") else 0
        s = Sampler(srv.ps)
        s.start()
        res = loadgen(srv.port, scenario["args"] + ["-d", f"{seconds}s"], srv.ps)
        s.stop = True
        s.join()
        after = srv.memory()
        go1 = srv.go_stats()
        res["start_rss_mb"] = before_mem.rss / 2**20
        res["peak_rss_mb"] = max(s.peak_rss, getattr(after, "peak_wset", 0)) / 2**20
        res["peak_private_mb"] = s.peak_private / 2**20
        res["peak_threads"] = s.peak_threads
        total = max(1, res["total_with_warmup"])
        if hasattr(after, "num_page_faults"):
            res["page_faults_per_1k_requests"] = (after.num_page_faults - faults0) * 1000 / total
        if go0 and go1:
            res["go_mallocs_per_request"] = (go1["Mallocs"] - go0["Mallocs"]) / total
            res["go_bytes_per_request"] = (go1["TotalAlloc"] - go0["TotalAlloc"]) / total
            res["go_gcs"] = go1["NumGC"] - go0["NumGC"]
            res["go_gc_pause_ms"] = (go1["PauseTotalNs"] - go0["PauseTotalNs"]) / 1e6
    finally:
        code, out, err = srv.stop()
    res["exit"] = code
    res["server_said"] = out.strip().splitlines()[-1] if out.strip() else ""
    if err.strip():
        res["server_stderr"] = err.strip()[-300:]
    return res

def idle_memory(kind, n):
    """Server memory with n idle keep-alive connections, each having made
    one request, against none. Opened one after another and measured at
    once, well inside both servers' 5 s idle timeout."""
    srv = Server(kind)
    socks = []
    try:
        time.sleep(0.3)
        base = srv.memory()
        t0 = time.time()
        slowest = (0, 0, "")
        for i in range(n):
            a = time.perf_counter()
            # Blocking, with no timeout: on Windows, a socket with a timeout
            # waits in select() on the 15.6 ms system timer, which made each
            # connect here take a tick, whatever the server.
            s = socket.create_connection(("127.0.0.1", srv.port))
            b = time.perf_counter()
            s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
            s.recv(4096)
            c = time.perf_counter()
            socks.append(s)
            if c - a > slowest[0]:
                slowest = (c - a, i, f"connect {1000 * (b - a):.1f} ms, response {1000 * (c - b):.1f} ms")
        time.sleep(0.2)
        m = srv.memory()
        threads = srv.ps.num_threads()
        if time.time() - t0 > 4:
            raise RuntimeError(f"opening {n} connections took {time.time() - t0:.1f} s, past the idle timeout; "
                               f"slowest was #{slowest[1]}: {slowest[2]}")
        res = {"connections": n, "base_rss_mb": base.rss / 2**20, "rss_mb": m.rss / 2**20,
               "rss_kb_per_connection": (m.rss - base.rss) / 1024 / n,
               "private_kb_per_connection": (getattr(m, "private", m.vms) - getattr(base, "private", base.vms)) / 1024 / n,
               "threads": threads}
    finally:
        for s in socks:
            s.close()
        srv.stop()
    return res

SCENARIOS = [
    {"name": "mixed, 16 connections", "args": ["-mix", "mixed", "-c", "16"]},
    {"name": "mixed, 64 connections", "args": ["-mix", "mixed", "-c", "64"]},
    {"name": "mixed, 256 connections", "args": ["-mix", "mixed", "-c", "256"]},
    {"name": "health, 64 connections", "args": ["-mix", "health", "-c", "64"]},
    {"name": "order, 64 connections", "args": ["-mix", "order", "-c", "64"]},
    {"name": "mixed, 64 connections, 20k req/s fixed rate", "args": ["-mix", "mixed", "-c", "64", "-rate", "20000"]},
    {"name": "mixed, new connection per request, 16 clients", "args": ["-mix", "mixed", "-c", "16", "-churn"]},
]

KINDS = ["gust", "go", "goraw"]
# The executable each server kind runs, and the arguments it gets after its port.
EXES = {"gust": "api_gust", "go": "server_go", "goraw": "rawserver_go"}
SERVER_ARGS = {}

KEYS = ["rps", "p50_us", "p90_us", "p99_us", "p999_us", "max_us", "due_p50_us", "due_p99_us", "due_p999_us",
        "server_cpu_share", "client_cpu_share", "peak_rss_mb", "peak_private_mb", "peak_threads",
        "page_faults_per_1k_requests", "go_mallocs_per_request", "go_bytes_per_request", "go_gcs", "go_gc_pause_ms"]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--seconds", type=int, default=10)
    ap.add_argument("--quick", action="store_true", help="one rep of 3 s, for a smoke test")
    ap.add_argument("--only", help="run the scenarios whose name contains this")
    ap.add_argument("--out", default=os.path.join(OUT, "results.json"))
    args = ap.parse_args()
    if args.quick:
        args.reps, args.seconds = 1, 3
    build()
    scenarios = [s for s in SCENARIOS if not args.only or args.only in s["name"]]
    report = {"machine": {"cpu": "see docs", "logical_cpus": psutil.cpu_count(), "server_cpus": SERVER_CPUS,
                          "client_cpus": CLIENT_CPUS},
              "go": subprocess.run(["go", "version"], capture_output=True, text=True).stdout.strip(),
              "scenarios": []}
    # First: right after the churn scenario, the system is still tearing
    # down thousands of reset connections, and new ones open slowly.
    report["idle"] = {}
    for kind in KINDS:
        report["idle"][kind] = idle_memory(kind, 500)
        print(f"idle 500 connections {kind}: {report['idle'][kind]}", flush=True)
    for sc in scenarios:
        runs = {kind: [] for kind in KINDS}
        for rep in range(args.reps):
            # Each rep starts with a different server, so no server always
            # runs first or last.
            for kind in KINDS[rep % len(KINDS):] + KINDS[:rep % len(KINDS)]:
                r = run_one(kind, sc, args.seconds)
                runs[kind].append(r)
                print(f"{sc['name']:50s} {kind:5s} rep {rep + 1}: {r['rps']:9.0f} req/s  p50 {r['p50_us']:7.0f} us  "
                      f"p99 {r['p99_us']:7.0f}  p99.9 {r['p999_us']:7.0f}  rss {r['peak_rss_mb']:6.1f} MB  "
                      f"cpu server {r['server_cpu_share']:4.0%} client {r['client_cpu_share']:4.0%}  "
                      f"errors {r['errors']} mismatches {r['status_mismatches']}", flush=True)
        summary = {}
        for kind, rs in runs.items():
            summary[kind] = {k: statistics.median([r[k] for r in rs]) for k in KEYS if all(k in r for r in rs)}
            summary[kind]["errors"] = sum(r["errors"] for r in rs)
            summary[kind]["status_mismatches"] = sum(r["status_mismatches"] for r in rs)
        report["scenarios"].append({"name": sc["name"], "args": sc["args"], "median": summary, "runs": runs})
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(report, f, indent=1)
    print("wrote", args.out)

if __name__ == "__main__":
    main()
