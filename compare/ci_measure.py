#!/usr/bin/env python3
"""Gust: the compare suite's timings on a CI runner, for the ratios between
builds measured side by side: gust run --mir against the native build, and
Gust's native builds against Rust's. Each benchmark's builds run in turn,
--runs times, and the fastest of each counts, which a runner's noisy
neighbours slow least. A runner's absolute times mean little; the ratios
between programs run on it, alternately, are what this is for.

    python compare/ci_measure.py --gust build/gust --out dir [--runs 7]
"""
import argparse, math, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
BENCHES = ["nbody", "raytrace", "interp", "sort", "astar", "bintrees", "hashmap", "strings"]


def sh(argv, **kw):
    r = subprocess.run(argv, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.exit(f"{' '.join(map(str, argv))} failed:\n{r.stdout}{r.stderr}")
    return r.stdout


def timed(argv, cwd=None):
    """The benchmark's own time, from its "bench" line."""
    for line in sh(argv, cwd=cwd).splitlines():
        if line.startswith("bench"):
            return float(line.split("\t")[2])
    sys.exit(f"no bench line from {' '.join(map(str, argv))}")


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

    builds = {}
    for cc in ("clang", "gcc") + (("msvc",) if os.name == "nt" else ()):
        path = os.path.join(out, f"gust-{cc}{exe}")
        r = subprocess.run([gust, "build", "-q", "--cc", cc, src, "-o", path], capture_output=True, text=True)
        if r.returncode == 0:
            builds[f"gust/{cc}"] = [path]
        else:
            print(f"no gust/{cc} build: {(r.stdout + r.stderr).strip().splitlines()[-1:]}")
    if shutil.which("rustc"):
        path = os.path.join(out, f"rust-o3-lto{exe}")
        sh(["rustc", "-C", "debuginfo=0", "-C", "opt-level=3", "-C", "lto=fat", "-C", "codegen-units=1",
            "-o", path, os.path.join(HERE, "rust", "main.rs")])
        builds["rust"] = [path]
    if not args.no_mir:
        builds["gust/mir"] = [gust, "run", "--mir", "-q", src, "--"]

    best = {(name, b): math.inf for name in builds for b in BENCHES}
    for b in BENCHES:
        for _ in range(args.runs):
            for name, argv in builds.items():
                best[name, b] = min(best[name, b], timed(argv + ["1", b]))

    names = list(builds)
    print("bench     " + "".join(f"{n:>12}" for n in names))
    for b in BENCHES:
        print(f"{b:<10}" + "".join(f"{best[n, b]:12.4f}" for n in names))

    def ratios(a, base):
        if a not in builds or base not in builds:
            return
        print(f"\n{a} / {base} (below 1: {a} faster)")
        logs = 0.0
        for b in BENCHES:
            r = best[a, b] / best[base, b]
            logs += math.log(r)
            print(f"  {b:<10}{r:6.2f}x")
        print(f"  geomean   {math.exp(logs / len(BENCHES)):6.3f}x")

    ratios("gust/mir", "gust/gcc")
    ratios("gust/mir", "gust/clang")
    ratios("gust/clang", "rust")
    ratios("gust/gcc", "rust")
    ratios("gust/msvc", "rust")


if __name__ == "__main__":
    main()
