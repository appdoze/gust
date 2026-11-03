"""Temporary: main against the change for more threads than CPUs, and the change without its guard on
giving up a turn, round by round on one machine, each compiled the same way.

    python qcrowd.py <new gust> <old gust> [--out dir] [--rounds n]
"""
import argparse
import ctypes
import glob
import os
import platform
import shutil
import statistics
import subprocess
import sys
import time

ap = argparse.ArgumentParser()
ap.add_argument("new")
ap.add_argument("old")
ap.add_argument("--out", default=os.environ.get("RUNNER_TEMP", "qw"))
ap.add_argument("--rounds", type=int, default=11)
ap.add_argument("--clang", default="clang")
args = ap.parse_args()

WINDOWS = os.name == "nt"
EXE = ".exe" if WINDOWS else ""
OUT = os.path.abspath(args.out)
os.makedirs(OUT, exist_ok=True)
HOME = os.path.expanduser("~")
SRC = "compare/concurrency"
FLAGS = "-O2 -ffunction-sections -fdata-sections -fno-stack-protector -w".split()
LIBS = [] if WINDOWS else ["-lpthread", "-lm"]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def sources(gust):
    """The program's C and the runtime's, as this compiler writes them."""
    found = {}
    for src in (f"{SRC}/conc.goose", ".github/probe/qpair.goose"):
        stem = os.path.basename(src)[:-len(".goose")]
        r = run([os.path.abspath(gust), "build", "-q", "--cc", "clang", src, "-o", f"{OUT}/{stem}_plain{EXE}"])
        assert r.returncode == 0, r.stdout + r.stderr
        found[stem] = max(glob.glob(f"{HOME}/.gust/build/prog/{stem}-*/{stem}.c"), key=os.path.getmtime)
        found[stem + "_c"] = open(found[stem], encoding="utf-8", errors="replace").read()
    rt = max(glob.glob(f"{HOME}/.gust/build/runtime/*/clang*release*/goose_runtime.c"), key=os.path.getmtime)
    found["rt"] = rt
    found["rt_c"] = open(rt, encoding="utf-8", errors="replace").read().replace("\r\n", "\n")
    return found


def build(name, found, runtime):
    stem = name.replace(" ", "_").replace(",", "_")
    c = f"{OUT}/rt_{stem}.c"
    open(c, "w", encoding="utf-8").write(runtime)
    made = {}
    for prog in ("conc", "qpair"):
        src = f"{OUT}/{prog}_{stem}.c"
        open(src, "w", encoding="utf-8").write(found[prog + "_c"])
        exe = f"{OUT}/{prog}_{stem}{EXE}"
        r = run([args.clang, *FLAGS, f"-I{os.path.dirname(found['rt'])}", f"-I{os.path.dirname(found[prog])}", src, c, "-o", exe, *LIBS])
        if r.returncode:
            sys.exit(f"{name}: {r.stderr[-1500:]}")
        made[prog] = exe
    return made


old = sources(args.old)
new = sources(args.new)
GUARD = "        gs_qyield_rest = GS_QYIELD_REST;\n"
assert new["rt_c"].count(GUARD) == 1
builds = {
    "main": build("main", old, old["rt_c"]),
    "crowd": build("crowd", new, new["rt_c"]),
    "no guard": build("no guard", new, new["rt_c"].replace(GUARD, "        ;\n")),
}
progs = {n: b["conc"] for n, b in builds.items()}
pairs = {n: b["qpair"] for n, b in builds.items()}
if shutil.which("rustc") and shutil.which("go"):
    rust = f"{OUT}/conc_rust{EXE}"
    r = run(["rustc", "-C", "debuginfo=0", "-C", "opt-level=3", "-C", "lto=fat", "-C", "codegen-units=1", "-o", rust, f"{SRC}/conc.rs"])
    if r.returncode == 0:
        progs["rust"] = rust
    go = f"{OUT}/conc_go{EXE}"
    r = run(["go", "build", "-o", go, "."], cwd=f"{SRC}/go")
    if r.returncode == 0:
        progs["go"] = go


def once(exe, bench, cpus):
    argv = ["taskset", "-c", cpus, exe, "1", bench] if cpus and not WINDOWS else [exe, "1", bench]
    if WINDOWS:
        out = run(argv, timeout=600).stdout
        return float(out.split("\t")[2]), 0.0, 0
    p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    out = p.stdout.read()
    _, _, ru = os.wait4(p.pid, 0)
    p.returncode = 0
    return float(out.split("\t")[2]), ru.ru_utime + ru.ru_stime, ru.ru_nvcsw


def rounds(label, table, bench, cpus, count):
    rows = {n: [] for n in table}
    for _ in range(count):
        for n, exe in table.items():
            rows[n].append(once(exe, bench, cpus))
    print(f"\n{label}: {bench}", flush=True)
    ref = statistics.median(r[0] for r in rows["main"])
    for n, rs in rows.items():
        wall = sorted(r[0] for r in rs)
        print(f"  {n:<9} {statistics.median(wall):.4f} max {wall[-1]:.4f} {statistics.median(wall) / ref:6.2f}x"
              f" cpu {statistics.median(r[1] for r in rs):.3f} sw {int(statistics.median(r[2] for r in rs))}", flush=True)


def limit(cpus):
    if WINDOWS:
        k = ctypes.windll.kernel32
        k.GetCurrentProcess.restype = ctypes.c_void_p
        k.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        mask = sum(1 << int(c) for c in cpus.split(",")) if cpus else (1 << os.cpu_count()) - 1
        assert k.SetProcessAffinityMask(k.GetCurrentProcess(), mask)


print(f"{platform.system()} {platform.machine()}, {os.cpu_count()} CPUs; {args.rounds} rounds, each build in turn; medians", flush=True)
if shutil.which("lscpu"):
    print(run("lscpu | grep -E 'Model name'", shell=True).stdout.strip(), flush=True)
start = time.time()
limit("")
for b in ("fanout", "pingpong", "spawnjoin", "spawnmany", "spawnwork"):
    rounds("all CPUs", progs, b, "", args.rounds)
for b in ("idlepair", "busypair"):
    rounds("all CPUs", pairs, b, "", args.rounds)
if WINDOWS or shutil.which("taskset"):
    few = max(args.rounds // 2, 5)
    for label, cpus, conc, pair in (
        ("two CPUs, two cores", "0,2", ("fanout", "pingpong"), ("idlepair", "busypair")),
        ("two CPUs, one core", "0,1", ("fanout",), ()),
        ("one CPU", "0", ("fanout", "pingpong", "spawnjoin", "spawnwork"), ("idlepair", "busypair")),
    ):
        limit(cpus)
        for b in conc:
            rounds(label, progs, b, cpus, few)
        for b in pair:
            rounds(label, pairs, b, cpus, few)
print(f"\n{time.time() - start:.0f} s", flush=True)
