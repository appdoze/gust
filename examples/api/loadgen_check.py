"""Is the load generator the bottleneck? The same 64 connections driven by
one load generator process, then split over two processes of 32 each, on
the same client CPUs, against each server. If the load generator limited
the throughput, two processes would get more out of the same server; if
the server (or the loopback path between them) did, the total stays.

    python examples/api/loadgen_check.py [--kinds gust,goraw] [--seconds 6]
"""
import argparse, json, os, subprocess, sys, threading, time
import psutil

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bench  # noqa: E402

def run_loadgens(port, parts, seconds):
    exe = os.path.join(bench.OUT, "loadgen_go" + bench.EXE)
    procs = []
    for conns in parts:
        p = subprocess.Popen([exe, "-addr", f"127.0.0.1:{port}", "-mix", "mixed", "-c", str(conns),
                              "-d", f"{seconds}s", "-warmup", "1s"], stdout=subprocess.PIPE,
                             env=dict(os.environ, GOMAXPROCS="8"))
        psutil.Process(p.pid).cpu_affinity(bench.CLIENT_CPUS)
        procs.append(p)
    results = [json.loads(p.communicate()[0]) for p in procs]
    return sum(r["rps"] for r in results), results

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kinds", default="gust,goraw")
    ap.add_argument("--seconds", type=int, default=6)
    ap.add_argument("--reps", type=int, default=2)
    args = ap.parse_args()
    for kind in args.kinds.split(","):
        for rep in range(args.reps):
            for parts in ([64], [32, 32]):
                srv = bench.Server(kind)
                try:
                    total, results = run_loadgens(srv.port, parts, args.seconds)
                finally:
                    srv.stop()
                gcs = sum(r["loadgen_gcs"] for r in results)
                pause = sum(r["loadgen_gc_pause_ms"] for r in results)
                print(f"{kind:12s} rep {rep + 1}: {len(parts)} load generator(s) x {parts[0]} connections: "
                      f"{total:9.0f} req/s in all; load generator collections {gcs}, {pause:.1f} ms paused",
                      flush=True)

if __name__ == "__main__":
    main()
