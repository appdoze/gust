#!/usr/bin/env python3
"""Gust: the programs that use threads, and the runtime with them, under
ThreadSanitizer. The rest of CI runs them on real threads, but a race only shows
there where it happens to go wrong, and on x86 most never do.

Each program's C and the runtime's, as the compiler wrote them, are compiled with
clang's -fsanitize=thread and run; any report, a wrong exit or a wrong output fails.
Programs expected to abort are left out, as what they test is the abort.

    python scripts/tsan_threads.py --gust build/gust [name ...]
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOME = os.path.expanduser("~")
EXTRA = ["test/codegen/parallel_for.goose"]


def newest(pattern):
    found = glob.glob(pattern)
    return max(found, key=os.path.getmtime) if found else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gust", required=True)
    ap.add_argument("names", nargs="*", help="only these programs")
    args = ap.parse_args()
    gust = os.path.abspath(args.gust)
    os.chdir(ROOT)
    programs = sorted(glob.glob("test/threads/*.goose")) + EXTRA
    env = {**os.environ, "TSAN_OPTIONS": "halt_on_error=0 exitcode=66 report_signal_unsafe=0"}
    out = tempfile.mkdtemp(prefix="tsan")
    bad = []
    ran = 0
    for src in programs:
        name = os.path.basename(src)[:-len(".goose")]
        if args.names and name not in args.names:
            continue
        if os.path.exists(f"test/expected/{name}.aborts") or not os.path.exists(f"test/expected/{name}.out"):
            continue
        first = open(src, encoding="utf-8").readline()
        defines = [f"-D{d}" for d in re.findall(r"^// runtime-define: (\w+=\w+)$", first)]
        if first.startswith("// runtime-debug"):
            defines.append("-DGS_DEBUG=1")
        r = subprocess.run([gust, "build", "-q", "--cc", "clang", src, "-o", os.path.join(out, name + "_plain")],
                           capture_output=True, text=True)
        prog = newest(f"{HOME}/.gust/build/prog/{name}-*/{name}.c")
        runtime = newest(f"{HOME}/.gust/build/runtime/*/clang*release*/goose_runtime.c")
        if r.returncode or not prog or not runtime:
            bad.append(name)
            print(f"{name}: the compiler failed\n{r.stdout}{r.stderr}", flush=True)
            continue
        exe = os.path.join(out, name)
        r = subprocess.run(["clang", "-fsanitize=thread", "-O1", "-g", "-w", *defines, prog, runtime, "-o", exe,
                            "-lpthread", "-lm"], capture_output=True, text=True)
        if r.returncode:
            bad.append(name)
            print(f"{name}: clang failed\n{r.stderr[-1500:]}", flush=True)
            continue
        t0 = time.time()
        try:
            r = subprocess.run([exe], env=env, capture_output=True, text=True, timeout=600, stdin=subprocess.DEVNULL)
        except subprocess.TimeoutExpired:
            bad.append(name)
            print(f"{name}: no end after 600 s", flush=True)
            continue
        ran += 1
        reports = r.stderr.count("WARNING: ThreadSanitizer")
        want = open(f"test/expected/{name}.out", encoding="utf-8").read()
        right = r.stdout == want
        print(f"{name}: {reports} reports, exit {r.returncode}, output {'right' if right else 'WRONG'}, {time.time() - t0:.1f} s", flush=True)
        if reports or r.returncode or not right:
            bad.append(name)
            print(r.stderr[:5000], flush=True)
    print(f"{ran} programs under ThreadSanitizer, {len(bad)} with a problem: {bad}", flush=True)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
