"""Temporary: spinning less when more threads are running than there are CPUs, counting only those
not blocked, and giving the CPU away before sleeping. Each round by round on one machine, every Gust
build compiled the same way, beside idle and busy neighbours.

    python qrun_probe.py <gust> [--out dir] [--rounds n] [--check dir]
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
ap.add_argument("gust", nargs="?")
ap.add_argument("--out", default=os.environ.get("RUNNER_TEMP", "qw"))
ap.add_argument("--rounds", type=int, default=11)
ap.add_argument("--clang", default="clang")
ap.add_argument("--check", help="only see that every patch applies to src/runtime here")
args = ap.parse_args()

WINDOWS = os.name == "nt"
EXE = ".exe" if WINDOWS else ""
SPIN_OLD = """    for (int spins = gs_spin_for(GS_QSPIN); spins > 0; spins--) {
        if (gs_qany(s)) {
            gs_qnode *n = gs_qtake_now(s);
            if (n) return n;
            for (int r = 0; r < GS_QLOST; r++) gs_pf_relax();
        }
        gs_pf_relax();
    }
"""


def counting(name, down, up):
    """The count of threads running: each worker's job, less those blocked."""
    return [
        ("static int64_t gs_cpus_seen;\n", f"""static int64_t gs_cpus_seen;
static int64_t gsq_running = 1;
#define GSQ_BLOCKS() __atomic_sub_fetch(&gsq_running, 1, __ATOMIC_RELAXED)
#define GSQ_RUNS() __atomic_add_fetch(&gsq_running, 1, __ATOMIC_RELAXED)
#ifdef _WIN32
#define GSQ_YIELD() SwitchToThread()
#else
#define GSQ_YIELD() sched_yield()
#endif
"""),
        ("    return cpus > 1 ? looks : 1;\n", """    if (cpus <= 1) return 1;
    return __atomic_load_n(&gsq_running, __ATOMIC_RELAXED) > cpus ? (looks >> 5) + 1 : looks;
"""),
        ("        gs_regions_begin(gs_workerregions);\n        gs_current_thread_id = t->id;\n",
         "        GSQ_RUNS();\n        gs_regions_begin(gs_workerregions);\n        gs_current_thread_id = t->id;\n"),
        ("        gs_mutex_lock(&gs_threads_mutex);\n        gs_thread **slot = &gs_threads;\n",
         "        GSQ_BLOCKS();\n        gs_mutex_lock(&gs_threads_mutex);\n        gs_thread **slot = &gs_threads;\n"),
    ] + ([
        ("            gs_cond_wait(&s->cond, &s->mutex);\n", f"            {down}\n            gs_cond_wait(&s->cond, &s->mutex);\n            {up}\n"),
        ("                waiting = gs_cond_wait_until(&s->cond, &s->mutex, deadline);\n",
         f"                {down}\n                waiting = gs_cond_wait_until(&s->cond, &s->mutex, deadline);\n                {up}\n"),
        ("        gs_cond_wait(&gs_threads_done, &gs_threads_mutex);\n", f"        {down}\n        gs_cond_wait(&gs_threads_done, &gs_threads_mutex);\n        {up}\n"),
    ] if down else [])


RUN = counting("run", "GSQ_BLOCKS();", "GSQ_RUNS();")
ALIVE = counting("alive", "", "")
LOST = [("            for (int r = 0; r < GS_QLOST; r++) gs_pf_relax();\n", "            for (int r = gs_spin_for(GS_QLOST); r > 0; r--) gs_pf_relax();\n")]


def yields(n):
    """Over the spin, with more running than CPUs: give the CPU away before sleeping."""
    return [(SPIN_OLD, SPIN_OLD + f"""    if (gs_spin_for(32) != 32) {{
        for (int y = 0; y < {n}; y++) {{
            GSQ_YIELD();
            if (gs_qany(s)) {{
                gs_qnode *n = gs_qtake_now(s);
                if (n) return n;
            }}
        }}
    }}
""")]


VARIANTS = {
    "main": [],
    "alive": ALIVE,
    "running": RUN,
    "running,1 yield": RUN + yields(1) ,
    "running,4 yields": RUN + yields(4),
    "running,4,lost": RUN + yields(4) + LOST,
}


def apply(text, pairs):
    for old, new, *many in pairs:
        n = text.count(old)
        if n < 1 or (n != 1 and not many):
            sys.exit(f"{n} matches for {old[:70]!r}")
        text = text.replace(old, new)
    return text


if args.check:
    head = open(os.path.join(args.check, "runtime_threads.h"), encoding="utf-8").read().replace("\r\n", "\n")
    for name, pairs in VARIANTS.items():
        apply(head, pairs)
        print("applies:", name)
    sys.exit(0)

OUT = os.path.abspath(args.out)
os.makedirs(OUT, exist_ok=True)
HOME = os.path.expanduser("~")
SRC = "compare/concurrency"
FLAGS = "-O2 -ffunction-sections -fdata-sections -fno-stack-protector -w".split()
LIBS = [] if WINDOWS else ["-lpthread", "-lm"]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def latest(stem):
    return max(glob.glob(f"{HOME}/.gust/build/prog/{stem}-*/{stem}.c"), key=os.path.getmtime)


gust = os.path.abspath(args.gust)
for src in (f"{SRC}/conc.goose", ".github/probe/qpair.goose"):
    stem = os.path.basename(src)[:-len(".goose")]
    r = run([gust, "build", "-q", "--cc", "clang", src, "-o", f"{OUT}/{stem}_gust{EXE}"])
    assert r.returncode == 0, r.stdout + r.stderr
conc = latest("conc")
pair = latest("qpair")
rt = max(glob.glob(f"{HOME}/.gust/build/runtime/*/clang*release*/goose_runtime.c"), key=os.path.getmtime)
base = open(rt, encoding="utf-8", errors="replace").read().replace("\r\n", "\n")

progs = {}
pairs = {}
for name, patch in VARIANTS.items():
    stem = name.replace(" ", "_").replace(",", "_")
    c = f"{OUT}/rt_{stem}.c"
    open(c, "w", encoding="utf-8").write(apply(base, patch))
    for table, source, tag in ((progs, conc, "conc"), (pairs, pair, "pair")):
        exe = f"{OUT}/{tag}_{stem}{EXE}"
        r = run([args.clang, *FLAGS, f"-I{os.path.dirname(rt)}", source, c, "-o", exe, *LIBS])
        if r.returncode:
            sys.exit(f"{name}: {r.stderr[-1500:]}")
        table[name] = exe
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
        out = run(argv, timeout=300).stdout
        return float(out.split("\t")[2]), 0.0, 0, 0
    p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    out = p.stdout.read()
    _, _, ru = os.wait4(p.pid, 0)
    p.returncode = 0
    return float(out.split("\t")[2]), ru.ru_utime + ru.ru_stime, ru.ru_nvcsw, ru.ru_nivcsw


def rounds(label, table, bench, cpus, count):
    rows = {n: [] for n in table}
    for _ in range(count):
        for n, exe in table.items():
            rows[n].append(once(exe, bench, cpus))
    print(f"\n{label}: {bench}", flush=True)
    ref = statistics.median(r[0] for r in rows["main"])
    for n, rs in rows.items():
        wall = sorted(r[0] for r in rs)
        print(f"  {n:<17} {statistics.median(wall):.4f} max {wall[-1]:.4f} {statistics.median(wall) / ref:6.2f}x"
              f" | cpu {statistics.median(r[1] for r in rs):.3f}s"
              f" | switches {int(statistics.median(r[2] for r in rs))} asked {int(statistics.median(r[3] for r in rs))} forced", flush=True)


def limit(label, cpus):
    """Sets where children run on Windows; taskset does it elsewhere."""
    if WINDOWS:
        k = ctypes.windll.kernel32
        k.GetCurrentProcess.restype = ctypes.c_void_p
        k.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        mask = sum(1 << int(c) for c in cpus.split(",")) if cpus else (1 << os.cpu_count()) - 1
        assert k.SetProcessAffinityMask(k.GetCurrentProcess(), mask)


print(f"{platform.system()} {platform.machine()}, {os.cpu_count()} CPUs; {args.rounds} rounds, each build in turn; medians", flush=True)
if shutil.which("lscpu"):
    print(run("lscpu | grep -E 'Model name'; uname -r", shell=True).stdout.strip(), flush=True)
start = time.time()
few = args.rounds
limit("all", "")
for b in ("fanout", "pingpong", "spawnjoin", "spawnmany", "spawnwork"):
    rounds("all CPUs", progs, b, "", few)
for b in ("idlepair", "busypair"):
    rounds("all CPUs", pairs, b, "", few)
if WINDOWS or shutil.which("taskset"):
    for label, cpus, benches in (
        ("two CPUs, two cores", "0,2", ("fanout", "pingpong")),
        ("two CPUs, one core", "0,1", ("fanout",)),
        ("one CPU", "0", ("fanout", "pingpong", "spawnjoin", "spawnwork")),
    ):
        limit(label, cpus)
        for b in benches:
            rounds(label, progs, b, cpus, max(few // 2, 5))
        if "pingpong" in benches:
            rounds(label, pairs, "idlepair", cpus, max(few // 2, 5))
print(f"\n{time.time() - start:.0f} s", flush=True)
