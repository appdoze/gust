"""Gust's `parallel for` against Rust with rayon: nbody and raytrace, each
run sequentially and in parallel in one process (gust/parallel.goose,
rust_parallel/). Writes parallel_results.json beside this script (or --out).

    python compare/run_parallel.py [--runs N] [--repeats N] [--out path]
    python compare/run_parallel.py --check

Every parallel checksum must equal its sequential one exactly, which each
program's parallel form keeps by construction, and every configuration
must agree with the first (relative 1e-9). --check builds and runs each
configuration once and fails on any difference or on a Gust configuration
that does not build or run; timings from it mean nothing.

Rust builds where cargo is installed, from the registry or its offline
cache (rayon). Both sides use every hardware thread: Gust's workers are
one per hardware thread, rayon's pool is as many by default.
"""

import argparse
import json
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_bench import BUILD, EXE, GUST, HERE, machine, parse, run_measured, scrub_all, sh  # noqa: E402

BENCHES = ["nbody", "nbody_par", "raytrace", "raytrace_par"]
PAIRS = [("nbody", "nbody_par"), ("raytrace", "raytrace_par")]


def build_gust(name, cc):
    def go():
        out = os.path.join(BUILD, name + EXE)
        r, t = sh([GUST, "build", "-q", "--cc", cc, os.path.join(HERE, "gust", "parallel.goose"), "-o", out])
        if r.returncode != 0:
            raise RuntimeError((r.stderr or r.stdout)[-500:])
        return t, out
    return go


def build_rust():
    target = os.path.join(BUILD, "rust_parallel")
    manifest = os.path.join(HERE, "rust_parallel", "Cargo.toml")
    cmd = ["cargo", "build", "--release", "--manifest-path", manifest, "--target-dir", target]
    r, t = sh(cmd + ["--offline"])
    if r.returncode != 0:   # Not in the offline cache: from the registry.
        r, t = sh(cmd)
    if r.returncode != 0:
        raise RuntimeError(r.stderr[-500:])
    return t, os.path.join(target, "release", "rust_parallel" + EXE)


def configs():
    cs = [dict(name="gust/clang", lang="gust", build=build_gust("gust-parallel-clang", "clang"),
               note="gust build --cc clang")]
    if os.name == "nt":
        cs.append(dict(name="gust/msvc", lang="gust", build=build_gust("gust-parallel-msvc", "msvc"),
                       note="gust build --cc msvc"))
    if sys.platform.startswith("linux"):
        cs.append(dict(name="gust/gcc", lang="gust", build=build_gust("gust-parallel-gcc", "gcc"),
                       note="gust build --cc gcc"))
    if shutil.which("cargo"):
        cs.append(dict(name="rust/rayon", lang="rust", build=build_rust,
                       note="cargo build --release (opt-level 3), rayon"))
    return cs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--out", default=os.path.join(HERE, "parallel_results.json"))
    ap.add_argument("--check", action="store_true", help="build and run each once, check the checksums (CI)")
    args = ap.parse_args()
    if args.check:
        args.runs, args.repeats = 1, 1
    os.makedirs(BUILD, exist_ok=True)
    results = {"machine": machine(), "threads": os.cpu_count(), "runs": args.runs, "repeats": args.repeats,
               "generated": time.strftime("%Y-%m-%d %H:%M"), "configs": []}
    active = []
    for cfg in configs():
        rec = {"name": cfg["name"], "lang": cfg["lang"], "note": cfg["note"], "status": "ok", "seconds": {},
               "checksums": {}}
        results["configs"].append(rec)
        print(f"== build {cfg['name']}", flush=True)
        try:
            secs, exe = cfg["build"]()
            rec["build_seconds"] = secs
            active.append((rec, exe))
        except Exception as e:
            rec["status"] = f"build failed: {e}"
            print(f"   {rec['status']}", flush=True)
    # Round-robin, so that a patch of background load lands on all of them.
    for i in range(args.runs):
        for rec, exe in active:
            if rec["status"] != "ok":
                continue
            code, out, wall, peak = run_measured([exe, str(args.repeats)], HERE)
            times, sums = parse(out)
            if code != 0 or set(times) != set(BENCHES):
                rec["status"] = f"run failed: exit {code}: {out[-300:]}"
                print(f"   {rec['name']}: {rec['status']}", flush=True)
                continue
            for k, v in times.items():
                rec["seconds"][k] = min(rec["seconds"].get(k, v), v)
            rec["checksums"] = sums
            print(f"   run {i + 1} {rec['name']}: " + " ".join(f"{k}={v:.4f}" for k, v in times.items()), flush=True)
    problems = []
    for rec in results["configs"]:
        if rec["status"] != "ok":
            continue
        for seq, par in PAIRS:
            if rec["checksums"][seq] != rec["checksums"][par]:
                problems.append(f"{rec['name']}: {par} gave {rec['checksums'][par]!r}, {seq} "
                                f"{rec['checksums'][seq]!r}")
        rec["speedup"] = {par: rec["seconds"][seq] / rec["seconds"][par] for seq, par in PAIRS}
    ok = [c for c in results["configs"] if c["status"] == "ok"]
    for b in BENCHES:
        for c in ok[1:]:
            want, got = ok[0]["checksums"][b], c["checksums"][b]
            if abs(got - want) / max(abs(want), 1e-12) > 1e-9:
                problems.append(f"{b}: {c['name']} gave {got!r}, {ok[0]['name']} gave {want!r}")
    results["checksum_problems"] = problems
    if not args.check:
        with open(args.out, "w") as f:
            json.dump(scrub_all(results), f, indent=2)
        print("wrote", args.out)
    for c in ok:
        print(f"{c['name']:12} " + "  ".join(f"{b} {c['seconds'][b]:.4f}s" for b in BENCHES) +
              "  speedup " + " ".join(f"{k} {v:.2f}x" for k, v in c["speedup"].items()))
    print("checksum problems:", problems or "none")
    failed = [c["name"] for c in results["configs"] if c["lang"] == "gust" and c["status"] != "ok"]
    if failed:
        print("FAIL: Gust configuration(s) did not build or run:", ", ".join(failed))
    return 1 if problems or failed else 0


if __name__ == "__main__":
    sys.exit(main())
