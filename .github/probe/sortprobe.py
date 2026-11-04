"""Temporary: variants of std's sort round by round on one machine, each as its compiler builds it, beside
Rust, in several shapes; checks each with the edge cases; and where valgrind is there, counts instructions
and mispredicted branches for 3,000,000 random integers.

    python sortprobe.py <gust> <rounds> [--clang]
"""
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys

gust = os.path.abspath(sys.argv[1])
ROUNDS = int(sys.argv[2])
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.environ.get("RUNNER_TEMP", "."), "sp")
os.makedirs(OUT, exist_ok=True)
EXE = ".exe" if os.name == "nt" else ""
subprocess.run([sys.executable, os.path.join(HERE, "sortvar.py"), "stdlib", OUT], check=True)
names = sorted(d for d in os.listdir(OUT) if os.path.isdir(os.path.join(OUT, d)))


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


progs = {}
libs = {}
for n in names:
    env = {**os.environ, "GOOSE_STDLIB": os.path.join(OUT, n)}
    exe = os.path.join(OUT, f"shapes_{n}{EXE}")
    r = run([gust, "build", "-q", "--cc", "clang", os.path.join(HERE, "sortshapes.goose"), "-o", exe], env=env)
    if r.returncode:
        sys.exit(f"{n}: {(r.stdout + r.stderr)[-1500:]}")
    progs[n] = exe
    lib = os.path.join(OUT, f"lib_{n}{EXE}")
    r = run([gust, "build", "-q", "--cc", "clang", "compare/library/library.goose", "-o", lib], env=env)
    if r.returncode:
        sys.exit(f"{n}: {(r.stdout + r.stderr)[-1500:]}")
    libs[n] = lib
    chk = os.path.join(OUT, f"check_{n}{EXE}")
    r = run([gust, "build", "-q", "--cc", "clang", os.path.join(HERE, "sort_edges.goose"), "-o", chk], env=env)
    if r.returncode:
        sys.exit(f"{n}: {(r.stdout + r.stderr)[-1500:]}")
    r = run([chk], timeout=600)
    print(f"{n:<8} size {os.path.getsize(exe)} checker: {r.stdout.strip()} exit {r.returncode}", flush=True)
    # And the suite's own test of sorting, with the compiler's own checks in it.
    for test in ("test/stdlib/stdlib_sort.goose", "test/stdlib/stdlib_reference_elements.goose", "test/stdlib/stdlib_relative_elements.goose"):
        t = os.path.join(OUT, f"t_{n}{EXE}")
        r = run([gust, "build", "-q", "--cc", "clang", test, "-o", t], env=env)
        if r.returncode:
            sys.exit(f"{n} {test}: {(r.stdout + r.stderr)[-1500:]}")
        got = run([t], timeout=300).stdout.replace("\r\n", "\n")
        want = open("test/expected/" + os.path.basename(test)[:-len(".goose")] + ".out", encoding="utf-8").read().replace("\r\n", "\n")
        if got != want:
            print(f"  {n}: {test} output differs", flush=True)

if shutil.which("rustc"):
    rust = os.path.join(OUT, f"shapes_rust{EXE}")
    r = run(["rustc", "-C", "debuginfo=0", "-C", "opt-level=3", "-C", "lto=fat", "-C", "codegen-units=1", "-o", rust, os.path.join(HERE, "sortshapes.rs")])
    if r.returncode == 0:
        progs["rust"] = rust
    else:
        print("rust failed:", r.stderr[-800:])

print(f"\n{platform.system()} {platform.machine()}, {os.cpu_count()} CPUs; {ROUNDS} rounds, each build in turn; medians", flush=True)
if shutil.which("lscpu"):
    print(run("lscpu | grep -E 'Model name'", shell=True).stdout.strip(), flush=True)
for b in ("random", "small32", "reals", "tiny", "few", "nearly", "saw", "pipe", "keyed"):
    times = {n: [] for n in progs}
    sums = {}
    for _ in range(ROUNDS):
        for n, exe in progs.items():
            f = run([exe, "1", b], timeout=300).stdout.split("\t")
            times[n].append(float(f[2]))
            sums[n] = f[3].strip()
    if len(set(float(v) for v in sums.values())) != 1:
        print(f"  checksums differ on {b}: {sums}", flush=True)
    ref = statistics.median(times["main"])
    rust_t = statistics.median(times["rust"]) if "rust" in times else None
    print(f"\n{b}", flush=True)
    for n, v in times.items():
        v.sort()
        mid = statistics.median(v)
        vs = f" {mid / rust_t:5.2f}x of Rust" if rust_t else ""
        print(f"  {n:<8} {mid:.4f} ({v[0]:.4f}-{v[-1]:.4f}) {mid / ref:5.2f}x{vs}", flush=True)

times = {n: [] for n in libs}
for _ in range(ROUNDS):
    for n, exe in libs.items():
        times[n].append(float(run([exe, "1", "sortstr"], timeout=300).stdout.split("\t")[2]))
print("\nsortstr", flush=True)
ref = statistics.median(times["main"])
for n, v in times.items():
    v.sort()
    print(f"  {n:<8} {statistics.median(v):.4f} ({v[0]:.4f}-{v[-1]:.4f}) {statistics.median(v) / ref:5.2f}x", flush=True)

if shutil.which("valgrind"):
    print("\nrandom, cachegrind: instructions, branches mispredicted, millions", flush=True)
    for n, exe in progs.items():
        r = run(["valgrind", "--tool=cachegrind", "--cache-sim=no", "--branch-sim=yes", "--cachegrind-out-file=/dev/null", exe, "1", "random"], timeout=900)
        ir = re.search(r"I\s+refs:\s+([\d,]+)", r.stderr)
        mp = re.search(r"Mispredicts:\s+([\d,]+)", r.stderr)
        if ir and mp:
            print(f"  {n:<8} {int(ir.group(1).replace(',', '')) / 1e6:9.1f} {int(mp.group(1).replace(',', '')) / 1e6:8.2f}", flush=True)
        else:
            print(f"  {n:<8} {r.stderr[-300:]}", flush=True)
