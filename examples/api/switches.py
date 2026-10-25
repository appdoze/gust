"""OS context switches per request, and the server's threads, under a
closed-loop load. Counted system-wide (psutil.cpu_stats), because a
process's own count sums only the threads still alive and a Gust
connection's threads are gone when it closes: the load generator's share
is in every number, the same for every server.

    python examples/api/switches.py [--conns 64,256] [--kinds gust,goraw]

Explains tail latency at high connection counts: a server that runs a
thread per connection has the scheduler pick among that many runnable
threads on its CPUs, and pays a switch to wake the thread whose socket is
ready; one that multiplexes connections over a thread per CPU pays fewer.
"""
import argparse, os, sys, threading, time
import psutil

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bench  # noqa: E402

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--conns", default="64,256")
    ap.add_argument("--kinds", default="gust,goraw")
    ap.add_argument("--seconds", type=int, default=6)
    args = ap.parse_args()
    for kind in args.kinds.split(","):
        for c in (int(x) for x in args.conns.split(",")):
            srv = bench.Server(kind)
            peak = [0]
            def watch():
                while not done:
                    try:
                        peak[0] = max(peak[0], srv.ps.num_threads())
                    except psutil.Error:
                        return
                    time.sleep(0.1)
            done = False
            t = threading.Thread(target=watch, daemon=True)
            t.start()
            try:
                s0 = psutil.cpu_stats().ctx_switches
                res = bench.loadgen(srv.port, ["-mix", "mixed", "-c", str(c), "-d", f"{args.seconds}s",
                                               "-warmup", "1s"], srv.ps)
                s1 = psutil.cpu_stats().ctx_switches
            finally:
                done = True
                t.join()
                srv.stop()
            print(f"{kind:9s} c={c:4d}: {res['rps']:8.0f} req/s, p99 {res['p99_us']:7.0f} us, p99.9 "
                  f"{res['p999_us']:7.0f} us, {(s1 - s0) / max(1, res['total_with_warmup']):.2f} switches per "
                  f"request (system-wide), {peak[0]} threads at most, server CPU {res['server_cpu_share']:.0%}",
                  flush=True)

if __name__ == "__main__":
    main()
