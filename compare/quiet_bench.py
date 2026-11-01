#!/usr/bin/env python3
"""Gust: compare/'s benchmarks timed on a busy desktop, for A/B decisions
between builds, without closing anything or changing a system setting.

Other programs, the scheduler moving a process between cores, and the CPU's
clock following load and temperature all move a single timing by several
percent. What this does about it, within one process's own reach:

  * every run is pinned to one logical core, which the children inherit,
    so it is never migrated and loads only that core: by default the first
    of the two on the physical core the rest of the machine used least over
    two seconds (with psutil; --core names one);
  * the builds run in rounds, in a fresh random order each round, so a
    slow stretch of the machine falls on all of them alike;
  * the figure that counts is each round's ratio of one build's time to the
    first's, taken seconds apart, whose drift cancels; the median of those
    ratios is reported with its spread (the middle half of the rounds);
  * besides the benchmark's own time it reads the process's CPU cycles
    (Windows' QueryProcessCycleTime; user and system time elsewhere), which
    a stretch spent waiting for the core does not count, and the ratios of
    those too. Other programs on the same physical core's other thread, or
    in its caches, still slow it: the spread says how much.

    python compare/quiet_bench.py [--rounds 15] [--core N] [--benches nbody,raytrace] exe1 exe2 ...

Each exe is a build of compare/gust/main.goose or compare/rust/main.rs,
run as `exe 1 <bench>`. The first is the baseline the others are compared
with. Checksums are compared too: a build whose checksum differs from the
first's is flagged.
"""
import argparse, os, random, statistics, subprocess, sys

BENCHES = ["nbody", "raytrace", "interp", "sort", "astar", "bintrees", "hashmap", "strings"]


def quiet_core():
    """The first logical core of the physical core least used over two
    seconds, siblings taken to be numbered 2k and 2k + 1; 6 without psutil."""
    try:
        import psutil
    except ImportError:
        return 6
    load = psutil.cpu_percent(interval=2.0, percpu=True)
    if len(load) < 2:
        return 0
    pairs = [(load[i] + (load[i + 1] if i + 1 < len(load) else 0), i) for i in range(2, len(load), 2)]
    return min(pairs)[1]


def pin(core):
    """Pins this process, and so every child it starts, to one logical core."""
    if os.name == "nt":
        import ctypes
        k = ctypes.windll.kernel32
        k.GetCurrentProcess.restype = ctypes.c_void_p
        if not k.SetProcessAffinityMask(ctypes.c_void_p(k.GetCurrentProcess()), ctypes.c_size_t(1 << core)):
            sys.exit(f"cannot pin to core {core}")
    elif hasattr(os, "sched_setaffinity"):
        os.sched_setaffinity(0, {core})


def run(argv):
    """(bench seconds, checksum, CPU cost, peak memory bytes or None): the
    cost in millions of cycles on Windows, CPU seconds elsewhere."""
    p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    cpu = peak = None
    if os.name == "nt":
        out, err = p.communicate()
        import ctypes
        from ctypes import wintypes

        class Counters(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                        ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                        ("a", ctypes.c_size_t), ("b", ctypes.c_size_t), ("c", ctypes.c_size_t),
                        ("d", ctypes.c_size_t), ("e", ctypes.c_size_t), ("f", ctypes.c_size_t)]
        h = wintypes.HANDLE(int(p._handle))
        cycles = ctypes.c_uint64()
        if ctypes.windll.kernel32.QueryProcessCycleTime(h, ctypes.byref(cycles)):
            cpu = cycles.value / 1e6
        c = Counters()
        c.cb = ctypes.sizeof(c)
        if ctypes.windll.psapi.GetProcessMemoryInfo(h, ctypes.byref(c), c.cb):
            peak = c.PeakWorkingSetSize
        code = p.returncode
    else:
        out = p.stdout.read()
        err = p.stderr.read()
        _, status, ru = os.wait4(p.pid, 0)
        p.returncode = code = os.waitstatus_to_exitcode(status)
        cpu = ru.ru_utime + ru.ru_stime
    if code != 0:
        sys.exit(f"{' '.join(argv)} failed ({code}):\n{out}{err}")
    for line in out.splitlines():
        if line.startswith("bench"):
            f = line.split("\t")
            return float(f[2]), f[3].strip(), cpu, peak
    sys.exit(f"no bench line from {' '.join(argv)}")


def spread(xs):
    """The median and the middle half's width as a fraction of it."""
    xs = sorted(xs)
    med = statistics.median(xs)
    if len(xs) < 4:
        return med, 0.0
    q = statistics.quantiles(xs, n=4)
    return med, (q[2] - q[0]) / med if med else 0.0


def same(a, b):
    """Checksums equal as numbers (Rust prints 655421551704 where Gust prints 655421551704.0)."""
    try:
        return float(a) == float(b)
    except ValueError:
        return a == b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=15)
    ap.add_argument("--core", type=int, default=None)
    ap.add_argument("--benches", default=",".join(BENCHES))
    ap.add_argument("exes", nargs="+")
    args = ap.parse_args()
    core = quiet_core() if args.core is None else args.core
    pin(core)
    exes = [os.path.abspath(e) for e in args.exes]
    names = [os.path.basename(e) for e in exes]
    w = max(len(n) for n in names)
    unit = "Mcycles" if os.name == "nt" else "CPU s"
    print(f"pinned to core {core}; {args.rounds} rounds, builds in a random order each round;")
    print("ratios are each round's against the first build's, their median and the middle half's width")
    print(f"{'bench':<9} {'build':<{w}}  {'median s':>9} {'spread':>7}  {unit:>8}  {'peak MB':>7}  "
          f"{'time vs 1st':>12} {'CPU vs 1st':>12}")
    for b in args.benches.split(","):
        times = {e: [] for e in exes}
        cpus = {e: [] for e in exes}
        peaks = {e: [] for e in exes}
        sums = {}
        ratios = {e: [] for e in exes[1:]}
        cratios = {e: [] for e in exes[1:]}
        for _ in range(args.rounds):
            order = exes[:]
            random.shuffle(order)
            this, thisc = {}, {}
            for e in order:
                t, ck, cpu, peak = run([e, "1", b])
                this[e] = t
                thisc[e] = cpu
                times[e].append(t)
                if cpu is not None:
                    cpus[e].append(cpu)
                if peak is not None:
                    peaks[e].append(peak)
                sums.setdefault(e, ck)
            for e in exes[1:]:
                ratios[e].append(this[e] / this[exes[0]])
                if thisc[e] and thisc[exes[0]]:
                    cratios[e].append(thisc[e] / thisc[exes[0]])
        for e, n in zip(exes, names):
            med, sp = spread(times[e])
            cpu = f"{statistics.median(cpus[e]):8.1f}" if cpus[e] and os.name == "nt" else                 f"{statistics.median(cpus[e]):8.4f}" if cpus[e] else f"{'-':>8}"
            peak = f"{min(peaks[e]) / 1e6:7.2f}" if peaks[e] else f"{'-':>7}"
            if e == exes[0]:
                rel = f"{'':>12} {'':>12}"
            else:
                r, rs = spread(ratios[e])
                rel = f"{r:6.3f}x {rs * 100:4.1f}%"
                if cratios[e]:
                    c, cs = spread(cratios[e])
                    rel += f"  {c:6.3f}x {cs * 100:4.1f}%"
            flag = "" if same(sums[e], sums[exes[0]]) else f"  CHECKSUM {sums[e]} != {sums[exes[0]]}"
            print(f"{b:<9} {n:<{w}}  {med:9.4f} {sp * 100:6.1f}%  {cpu}  {peak}  {rel}{flag}")


if __name__ == "__main__":
    main()
