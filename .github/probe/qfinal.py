"""Temporary: main against the queue's small locks, each as its own compiler builds it, round by
round on one machine, with all the CPUs, two and one, beside Rust and Go.

    python qfinal.py <new gust> <old gust> [--out dir] [--rounds n]
"""
import argparse
import ctypes
import os
import platform
import shutil
import subprocess

ap = argparse.ArgumentParser()
ap.add_argument("new")
ap.add_argument("old")
ap.add_argument("--out", default=os.environ.get("RUNNER_TEMP", "qw"))
ap.add_argument("--rounds", type=int, default=25)
args = ap.parse_args()

WINDOWS = os.name == "nt"
EXE = ".exe" if WINDOWS else ""
OUT = os.path.abspath(args.out)
os.makedirs(OUT, exist_ok=True)
SRC = "compare/concurrency"


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


progs = {}
for name, gust in (("main", args.old), ("small locks", args.new)):
    exe = f"{OUT}/conc_{name.replace(' ', '_')}{EXE}"
    r = run([os.path.abspath(gust), "build", "-q", "--cc", "clang", f"{SRC}/conc.goose", "-o", exe])
    assert r.returncode == 0, r.stdout + r.stderr
    progs[name] = exe
if shutil.which("rustc") and shutil.which("go"):
    rust = f"{OUT}/conc_rust{EXE}"
    r = run(["rustc", "-C", "debuginfo=0", "-C", "opt-level=3", "-C", "lto=fat", "-C", "codegen-units=1", "-o", rust, f"{SRC}/conc.rs"])
    if r.returncode == 0:
        progs["rust"] = rust
    go = f"{OUT}/conc_go{EXE}"
    r = run(["go", "build", "-o", go, "."], cwd=f"{SRC}/go")
    if r.returncode == 0:
        progs["go"] = go


def rounds(bench, count, prefix=()):
    times = {n: [] for n in progs}
    for _ in range(count):
        for n, exe in progs.items():
            r = run([*prefix, exe, "1", bench], timeout=300)
            times[n].append(float(r.stdout.split("\t")[2]))
    mid = {n: sorted(v)[len(v) // 2] for n, v in times.items()}
    line = f"{bench:<10}"
    for n, v in times.items():
        line += f" | {n} {mid[n]:.4f} ({min(v):.4f}-{max(v):.4f})"
    print(line + f" | {mid['small locks'] / mid['main']:.2f}x of main", flush=True)


print(f"{platform.system()} {platform.machine()}, {os.cpu_count()} CPUs; medians of {args.rounds} rounds", flush=True)
if shutil.which("lscpu"):
    print(run("lscpu | grep -E 'Model name'", shell=True).stdout.strip(), flush=True)
ALL = ("fanout", "pingpong", "spawnjoin", "spawnmany", "spawnwork")
print("\nall CPUs", flush=True)
for b in ALL:
    rounds(b, args.rounds)

few = max(args.rounds // 4, 5)
if shutil.which("taskset"):
    for label, cpus in (("two CPUs, one core", "0,1"), ("two CPUs, two cores", "0,2"), ("one CPU", "0")):
        print(f"\n{label}", flush=True)
        for b in ALL:
            rounds(b, few, ("taskset", "-c", cpus))
elif WINDOWS:
    k = ctypes.windll.kernel32
    k.GetCurrentProcess.restype = ctypes.c_void_p
    k.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    for label, mask in (("two CPUs, one core", 0x3), ("two CPUs, two cores", 0x5), ("one CPU", 0x1)):
        assert k.SetProcessAffinityMask(k.GetCurrentProcess(), mask)
        print(f"\n{label}", flush=True)
        for b in ALL:
            rounds(b, few)
