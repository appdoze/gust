"""How each server's tail answers CPU contention from another process: the
same closed-loop run at 256 connections, alone and with busy-looping
processes pinned to some of the server's CPUs, alternating.

    python examples/api/contention.py [--kinds gust,goraw] [--hogs 2] [--reps 2]

A pool worker owns the connections it accepted, so while the OS runs
something else on its CPU, all of them wait; Go's scheduler lets another
thread take a stalled thread's runnable goroutines (work stealing).
"""
import argparse, multiprocessing, os, sys, time
import psutil

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bench  # noqa: E402

def spin():
    while True:
        pass

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kinds", default="gust,goraw")
    ap.add_argument("--hogs", type=int, default=2)
    ap.add_argument("--reps", type=int, default=2)
    ap.add_argument("--conns", type=int, default=256)
    args = ap.parse_args()
    # Busy loops on CPUs 0, 2, ...: different physical cores of the server's.
    hog_cpus = [bench.SERVER_CPUS[2 * i] for i in range(args.hogs)]
    for rep in range(args.reps):
        for loaded in (False, True):
            hogs = []
            if loaded:
                for cpu in hog_cpus:
                    p = multiprocessing.Process(target=spin, daemon=True)
                    p.start()
                    psutil.Process(p.pid).cpu_affinity([cpu])
                    hogs.append(p)
            try:
                for kind in args.kinds.split(","):
                    srv = bench.Server(kind)
                    try:
                        res = bench.loadgen(srv.port, ["-mix", "mixed", "-c", str(args.conns), "-d", "6s",
                                                       "-warmup", "1s"], srv.ps)
                    finally:
                        srv.stop()
                    what = f"busy loops on CPUs {hog_cpus}" if loaded else "no added load"
                    print(f"rep {rep + 1} {what:28s} {kind:6s}: {res['rps']:8.0f} req/s, p99 {res['p99_us']:7.0f} us, "
                          f"p99.9 {res['p999_us']:7.0f} us", flush=True)
            finally:
                for p in hogs:
                    p.terminate()

if __name__ == "__main__":
    main()
