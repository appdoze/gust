"""The pool at scale: the API server under the same measured load (mixed
requests on 64 keep-alive connections) beside 0, 1,000, 10,000 and 50,000
idle keep-alive connections, each pinging every 2 s, to see what an idle
connection costs a worker, with the platform's readiness backend (epoll,
kqueue, AFD; "native") and with poll / WSAPoll ("poll", GUST_NET_POLL=1),
which scans every socket on every wait.

    python examples/api/scale.py [--reps 3] [--seconds 10] [--idle 0,1000,10000,50000]
                                 [--backends native,poll]

Uses bench.py's builds (build/api-bench/), CPUs and sampling. Measured per
run: requests per second and latency percentiles (the 64 measured
connections only), the server's CPU per measured request, its peak working
set, the memory per idle connection against the run with none, and the
idle connections' failed pings. Writes build/api-bench/scale.json, or --out.
"""
import argparse, json, os, statistics, subprocess, sys, time

import psutil

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench


def run(backend, idle, seconds):
    background = psutil.cpu_percent(interval=1.0)
    os.environ.pop("GUST_NET_POLL", None)
    if backend == "poll":
        os.environ["GUST_NET_POLL"] = "1"    # bench.Server passes its environment on
    srv = bench.Server("gust")
    try:
        s = bench.Sampler(srv.ps)
        s.start()
        cpu0, t0 = bench.cpu_seconds(srv.ps), time.time()
        res = bench.loadgen(srv.port, ["-mix", "mixed", "-c", "64", "-d", f"{seconds}s", "-idle", str(idle)],
                            srv.ps)
        cpu = bench.cpu_seconds(srv.ps) - cpu0
        s.stop = True
        s.join()
        res["server_cpu_us_per_request"] = cpu / max(1, res["total_with_warmup"]) * 1e6
        res["peak_rss_mb"] = s.peak_rss / 2**20
        res["peak_private_mb"] = s.peak_private / 2**20
        res["background_cpu_pct"] = background
    finally:
        code, out, err = srv.stop()
    res["exit"] = code
    res["server_said"] = out.strip().splitlines()[-1] if out.strip() else ""
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--seconds", type=int, default=10)
    ap.add_argument("--idle", default="0,1000,10000,50000")
    ap.add_argument("--backends", default="native,poll")
    ap.add_argument("--no-build", action="store_true", help="use the builds already in build/api-bench/")
    ap.add_argument("--out", default=os.path.join(bench.OUT, "scale.json"))
    args = ap.parse_args()
    if not args.no_build:
        bench.build()
    counts = [int(x) for x in args.idle.split(",")]
    backends = args.backends.split(",")
    runs = {b: {n: [] for n in counts} for b in backends}
    for rep in range(args.reps):
        for b in backends:
            for n in counts[rep % len(counts):] + counts[:rep % len(counts)]:
                r = run(b, n, args.seconds)
                runs[b][n].append(r)
                print(f"{b:6} idle {n:6d} rep {rep + 1}: {r['rps']:9.0f} req/s  p50 {r['p50_us']:6.0f}  p99 {r['p99_us']:6.0f}  "
                      f"p99.9 {r['p999_us']:6.0f} us  cpu/request {r['server_cpu_us_per_request']:5.1f} us  "
                      f"rss {r['peak_rss_mb']:6.1f} MB  idle open {r.get('idle_connections', 0)} "
                      f"errors {r['errors']} idle errors {r.get('idle_errors', 0)}  "
                      f"background {r['background_cpu_pct']:.0f}%", flush=True)
    keys = ["rps", "p50_us", "p99_us", "p999_us", "server_cpu_us_per_request", "server_cpu_share", "peak_rss_mb",
            "peak_private_mb", "background_cpu_pct"]
    summary = {}
    for b in backends:
        sb = summary[b] = {}
        for n, rs in runs[b].items():
            sb[n] = {k: statistics.median(r[k] for r in rs) for k in keys}
            for k in ("rps", "p99_us", "p999_us"):
                sb[n][k + "_min"] = min(r[k] for r in rs)
                sb[n][k + "_max"] = max(r[k] for r in rs)
            sb[n]["errors"] = sum(r["errors"] for r in rs)
            sb[n]["idle_errors"] = sum(r.get("idle_errors", 0) for r in rs)
            sb[n]["idle_opened"] = min(r.get("idle_connections", 0) for r in rs)
        base = sb[min(counts)]
        for n in counts:
            if n:
                sb[n]["kb_per_idle_connection"] = (sb[n]["peak_private_mb"] - base["peak_private_mb"]) * 1024 / n
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        json.dump({"summary": summary, "runs": runs}, f, indent=1)
    print("wrote", args.out)


if __name__ == "__main__":
    main()
