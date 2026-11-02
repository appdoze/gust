#!/usr/bin/env python3
"""Gust: the test programs built with `gust build --target` (zig cc) and run, on a
machine of that target. The rest of the suite runs on what each runner has; this is what
says a static musl executable, or one for a system its C library differs on, behaves the
same. Programs expected to abort, and those that need what such a build leaves out
(libraries loaded at run time, a package's own C), are not run.

    python scripts/target_tests.py --gust build/gust --target x86_64-linux-musl
"""
import argparse
import concurrent.futures
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIRS = ["test/threads", "test/stdlib", "test/storage"]
# What a static executable, or a build with no packages, does not have.
NEEDS = re.compile(r"\b(cimport|dylib|import (gfx|audio|physics|ui|ffi))\b|extern\s+\"|c_headers")


def wanted():
    for d in DIRS:
        for src in sorted(glob.glob(f"{d}/*.goose")):
            name = os.path.basename(src)[:-len(".goose")]
            if os.path.exists(f"test/expected/{name}.aborts") or not os.path.exists(f"test/expected/{name}.out"):
                continue
            text = open(src, encoding="utf-8").read()
            first = text.split("\n", 1)[0]
            if first.startswith("// runtime-define") or NEEDS.search(text):
                continue
            yield src, name, first.startswith("// runtime-debug")


def one(job):
    (src, name, debug), gust, target, out = job
    exe = os.path.join(out, name + (".exe" if "windows" in target else ""))
    args = [gust, "build", "-q", "--target", target, *(["--debug"] if debug else []), src, "-o", exe]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode:
        return name, f"the build failed:\n{r.stdout[-1200:]}{r.stderr[-1200:]}"
    try:
        r = subprocess.run([exe], capture_output=True, timeout=300, stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return name, "no end after 300 s"
    want = open(f"test/expected/{name}.out", "rb").read().replace(b"\r\n", b"\n")
    got = r.stdout.replace(b"\r\n", b"\n")
    if r.returncode:
        return name, f"exit {r.returncode}: {r.stderr[-400:]!r}"
    if got != want:
        return name, f"the output differs: got {got[-200:]!r}, want {want[-200:]!r}"
    return name, None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gust", required=True)
    ap.add_argument("--target", required=True)
    ap.add_argument("-j", "--jobs", type=int, default=4)
    args = ap.parse_args()
    gust = os.path.abspath(args.gust)
    os.chdir(ROOT)
    out = tempfile.mkdtemp(prefix="target")
    # The first build makes the runtime object and zig's copy of the C library,
    # which the others then share.
    jobs = [(j, gust, args.target, out) for j in wanted()]
    bad = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        first = one(jobs[0])
        results = [first] + list(pool.map(one, jobs[1:]))
    for name, problem in results:
        if problem:
            bad.append(name)
            print(f"FAIL {name}: {problem}", flush=True)
    print(f"{len(results)} programs built for {args.target} and run, {len(bad)} failed: {bad}", flush=True)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
