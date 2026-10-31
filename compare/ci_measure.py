#!/usr/bin/env python3
"""Gust: the compare suite's timings on a CI runner, for the ratios between
builds measured side by side: gust run --mir against the native build, and
Gust's native builds against Rust's. Each benchmark's builds run in turn,
--runs times, and the fastest of each counts, which a runner's noisy
neighbours slow least. A runner's absolute times mean little; the ratios
between programs run on it, alternately, are what this is for.

Each run's peak memory is kept too (the least of its runs), and each
executable's size and build time, beside those of a hello world in Gust,
Rust and Go where the runner has them, each built as its toolchain does by
default. A Gust build's first time includes compiling the runtime object,
which later builds with the same toolchain reuse.

    python compare/ci_measure.py --gust build/gust --out dir [--runs 7]
"""
import argparse, math, os, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
BENCHES = ["nbody", "raytrace", "interp", "sort", "astar", "bintrees", "hashmap", "strings"]


def sh(argv, **kw):
    r = subprocess.run(argv, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.exit(f"{' '.join(map(str, argv))} failed:\n{r.stdout}{r.stderr}")
    return r.stdout


GNU_TIME = "/usr/bin/time" if sys.platform.startswith("linux") and os.path.exists("/usr/bin/time") else None


def run_measured(argv):
    """The program's output and its peak resident memory in bytes (None
    where the platform does not say). On Linux through GNU time, which
    forks it from a small process: a child forked from Python counts
    Python's pages in its maximum resident set until it execs."""
    if GNU_TIME:
        r = subprocess.run([GNU_TIME, "-f", "GUST_MAXRSS %M", *argv], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"{' '.join(map(str, argv))} failed:\n{r.stdout}{r.stderr}")
        lines = [l for l in r.stderr.splitlines() if l.startswith("GUST_MAXRSS ")]
        return r.stdout, int(lines[-1].split()[1]) * 1024 if lines else None
    p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if os.name == "nt":
        out, err = p.communicate()
        peak = None
        try:
            import ctypes
            from ctypes import wintypes

            class Counters(ctypes.Structure):
                _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                            ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                            ("QuotaPagedPoolUsage", ctypes.c_size_t),
                            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                            ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]
            c = Counters()
            c.cb = ctypes.sizeof(c)
            if ctypes.windll.psapi.GetProcessMemoryInfo(wintypes.HANDLE(int(p._handle)), ctypes.byref(c), c.cb):
                peak = c.PeakWorkingSetSize
        except Exception:
            peak = None
        code = p.returncode
    else:
        out = p.stdout.read()
        err = p.stderr.read()
        _, status, ru = os.wait4(p.pid, 0)
        p.returncode = code = os.waitstatus_to_exitcode(status)
        peak = ru.ru_maxrss * (1 if sys.platform == "darwin" else 1024)
    if code != 0:
        sys.exit(f"{' '.join(map(str, argv))} failed:\n{out}{err}")
    return out, peak


def timed(argv):
    """The benchmark's own time, from its "bench" line, and the run's peak memory."""
    out, peak = run_measured(argv)
    for line in out.splitlines():
        if line.startswith("bench"):
            return float(line.split("\t")[2]), peak
    sys.exit(f"no bench line from {' '.join(map(str, argv))}")


def build(argv, **kw):
    """Whether argv built, and its wall time in seconds."""
    t0 = time.perf_counter()
    r = subprocess.run(argv, capture_output=True, **kw)
    return r.returncode == 0, time.perf_counter() - t0


def hello_builds(gust, out, exe):
    """A hello world's executable in each language at hand, its default
    build: {name: (size, seconds)}. Gust's runtime objects are built already."""
    sizes = {}
    g = os.path.join(out, "hello.goose")
    open(g, "w").write('fn main() { print("hello"); }\n')
    for cc in ("clang", "gcc") + (("msvc",) if os.name == "nt" else ()):
        path = os.path.join(out, f"hello-gust-{cc}{exe}")
        ok, t = build([gust, "build", "-q", "--cc", cc, g, "-o", path])
        if ok:
            sizes[f"gust/{cc}"] = (os.path.getsize(path), t)
    if shutil.which("rustc"):
        r = os.path.join(out, "hello.rs")
        open(r, "w").write('fn main() { println!("hello"); }\n')
        path = os.path.join(out, f"hello-rust{exe}")
        ok, t = build(["rustc", "-C", "opt-level=3", "-o", path, r])
        if ok:
            sizes["rust"] = (os.path.getsize(path), t)
    if shutil.which("go"):
        d = os.path.join(out, "hello-go-src")
        os.makedirs(d, exist_ok=True)
        open(os.path.join(d, "main.go"), "w").write(
            'package main\n\nimport "fmt"\n\nfunc main() { fmt.Println("hello") }\n')
        open(os.path.join(d, "go.mod"), "w").write("module hello\n\ngo 1.21\n")
        path = os.path.join(out, f"hello-go{exe}")
        ok, t = build(["go", "build", "-o", path, "."], cwd=d)
        if ok:
            sizes["go"] = (os.path.getsize(path), t)
    return sizes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gust", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--runs", type=int, default=7)
    ap.add_argument("--no-mir", action="store_true")
    args = ap.parse_args()
    gust = os.path.abspath(args.gust)
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    exe = ".exe" if os.name == "nt" else ""
    src = os.path.join(HERE, "gust", "main.goose")

    builds, sizes, btimes = {}, {}, {}
    for cc in ("clang", "gcc") + (("msvc",) if os.name == "nt" else ()):
        path = os.path.join(out, f"gust-{cc}{exe}")
        argv = [gust, "build", "-q", "--cc", cc, src, "-o", path]
        t0 = time.perf_counter()
        r = subprocess.run(argv, capture_output=True, text=True)
        first = time.perf_counter() - t0
        if r.returncode == 0:
            builds[f"gust/{cc}"] = [path]
            sizes[f"gust/{cc}"] = os.path.getsize(path)
            btimes[f"gust/{cc}"] = (first, build(argv)[1])
        else:
            print(f"no gust/{cc} build:\n{r.stdout}{r.stderr}")
            for log in __import__("glob").glob(os.path.expanduser("~/.gust/build/prog/main-*/cc.log")):
                print(open(log, errors="replace").read()[-2000:])
    if shutil.which("rustc"):
        path = os.path.join(out, f"rust-o3-lto{exe}")
        t0 = time.perf_counter()
        sh(["rustc", "-C", "debuginfo=0", "-C", "opt-level=3", "-C", "lto=fat", "-C", "codegen-units=1",
            "-o", path, os.path.join(HERE, "rust", "main.rs")])
        builds["rust"] = [path]
        sizes["rust"] = os.path.getsize(path)
        btimes["rust"] = (time.perf_counter() - t0, None)
    if not args.no_mir:
        builds["gust/mir"] = [gust, "run", "--mir", "-q", src, "--"]

    best = {(name, b): math.inf for name in builds for b in BENCHES}
    peak = {(name, b): math.inf for name in builds for b in BENCHES}
    for b in BENCHES:
        for _ in range(args.runs):
            for name, argv in builds.items():
                t, m = timed(argv + ["1", b])
                best[name, b] = min(best[name, b], t)
                if m is not None:
                    peak[name, b] = min(peak[name, b], m)

    names = list(builds)
    print("bench     " + "".join(f"{n:>12}" for n in names))
    for b in BENCHES:
        print(f"{b:<10}" + "".join(f"{best[n, b]:12.4f}" for n in names))

    def ratios(a, base, table=best, what="time", lower="faster"):
        if a not in builds or base not in builds:
            return
        if any(math.isinf(table[n, b]) for n in (a, base) for b in BENCHES):
            return
        print(f"\n{a} / {base}, {what} (below 1: {a} {lower})")
        logs = 0.0
        for b in BENCHES:
            r = table[a, b] / table[base, b]
            logs += math.log(r)
            print(f"  {b:<10}{r:6.2f}x")
        print(f"  geomean   {math.exp(logs / len(BENCHES)):6.3f}x")

    ratios("gust/mir", "gust/gcc")
    ratios("gust/mir", "gust/clang")
    ratios("gust/clang", "rust")
    ratios("gust/gcc", "rust")
    ratios("gust/msvc", "rust")

    # Peak memory: a native build's own; gust/mir's includes the compiler.
    native = [n for n in names if n != "gust/mir"]
    print("\npeak memory, MB (least of the runs)")
    print("bench     " + "".join(f"{n:>12}" for n in native))
    for b in BENCHES:
        print(f"{b:<10}" + "".join(f"{peak[n, b] / 1e6:12.2f}" if not math.isinf(peak[n, b]) else f"{'-':>12}"
                                   for n in native))
    for n in native:
        if n != "rust":
            ratios(n, "rust", peak, "peak memory", "smaller")

    print("\nexecutable size, bytes, and build time: the compare program")
    print("(a Gust build's first time includes its runtime object, the second reuses it)")
    for n, s in sizes.items():
        first, again = btimes[n]
        print(f"  {n:<12}{s:>10}  {first:6.2f} s" + (f"  {again:6.2f} s" if again is not None else ""))
    print("executable size, bytes, and build time: hello world, each toolchain's default build")
    for n, (s, t) in hello_builds(gust, out, exe).items():
        print(f"  {n:<12}{s:>10}  {t:6.2f} s")


if __name__ == "__main__":
    main()
