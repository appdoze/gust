"""Temporary: a queue's sender not signalling a receiver already signalled, round by round on one
machine, every Gust build compiled the same way but the one-lock queue, which is as its compiler
builds it. With the counters of a run on what the sender did.

    python qwake_probe.py <new gust> [--old <old gust>] [--out dir] [--rounds n] [--mask m] [--clang path]
"""
import argparse
import ctypes
import glob
import os
import shutil
import subprocess
import sys

ap = argparse.ArgumentParser()
ap.add_argument("gust")
ap.add_argument("--old")
ap.add_argument("--out", default=os.environ.get("RUNNER_TEMP", "qw"))
ap.add_argument("--rounds", type=int, default=25)
ap.add_argument("--mask", type=lambda x: int(x, 0), default=0)
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


progs = {}
if args.old:
    exe = f"{OUT}/conc_one{EXE}"
    r = run([os.path.abspath(args.old), "build", "-q", "--cc", "clang", f"{SRC}/conc.goose", "-o", exe])
    assert r.returncode == 0, r.stdout + r.stderr
    progs["one lock"] = exe

r = run([os.path.abspath(args.gust), "build", "-q", "--cc", "clang", f"{SRC}/conc.goose", "-o", f"{OUT}/conc_gust{EXE}"])
assert r.returncode == 0, r.stdout + r.stderr
prog = max(glob.glob(f"{HOME}/.gust/build/prog/conc-*/conc.c"), key=os.path.getmtime)
rt = max(glob.glob(f"{HOME}/.gust/build/runtime/*/clang*release*/goose_runtime.c"), key=os.path.getmtime)
base = open(rt, encoding="utf-8", errors="replace").read()


def patched(pairs):
    t = base
    for old, new in pairs:
        if t.count(old) < 1:
            sys.exit(f"no match for {old[:70]!r}")
        t = t.replace(old, new)
    return t


COUNTED = [
    ("    int64_t sleeping;              /* receivers waiting on cond, or about to */\n",
     "    int64_t sleeping;              /* receivers waiting on cond, or about to */\n    int64_t signalled;\n"),
    ("    gs_qlock(&s->mutex);\n    gs_cond_signal(&s->cond);\n    gs_mutex_unlock(&s->mutex);\n",
     "    gs_qlock(&s->mutex);\n    if (s->sleeping > s->signalled) {\n        gs_qstore(&s->signalled, s->signalled + 1);\n        gs_cond_signal(&s->cond);\n    }\n    gs_mutex_unlock(&s->mutex);\n"),
    ("    if (GS_QSEQ_LOAD(&s->sleeping)) gs_qwake(s);\n",
     "    if (GS_QSEQ_LOAD(&s->sleeping) > gs_qload(&s->signalled)) gs_qwake(s);\n"),
    ("        if (GS_QSEQ_LOAD(&s->tail) == s->head) gs_cond_wait(&s->cond, &s->mutex);\n        GS_QSEQ_ADD(&s->sleeping, -1);\n",
     "        if (GS_QSEQ_LOAD(&s->tail) == s->head) {\n            gs_cond_wait(&s->cond, &s->mutex);\n            if (s->signalled) gs_qstore(&s->signalled, s->signalled - 1);\n        }\n        GS_QSEQ_ADD(&s->sleeping, -1);\n"),
    ("            if (GS_QSEQ_LOAD(&s->tail) == s->head)\n                waiting = gs_cond_wait_until(&s->cond, &s->mutex, deadline);\n            GS_QSEQ_ADD(&s->sleeping, -1);\n",
     "            if (GS_QSEQ_LOAD(&s->tail) == s->head) {\n                waiting = gs_cond_wait_until(&s->cond, &s->mutex, deadline);\n                if (s->signalled) gs_qstore(&s->signalled, s->signalled - 1);\n            }\n            GS_QSEQ_ADD(&s->sleeping, -1);\n"),
]
KEEP = [("        if (s->cap >= 65536 && h + 1 == s->seen_tail) {\n", "        if (0) {\n")]
COUNT = [
    ("#define GS_QLINE 64\n", """#define GS_QLINE 64
static long long gsq_grows, gsq_maxcap, gsq_frees, gsq_wakes, gsq_sleeps, gsq_lost, gsq_slowput;
#define GSQ(x) __atomic_add_fetch(&gsq_##x, 1, __ATOMIC_RELAXED)
"""),
    ("    int64_t cap = s->cap ? s->cap * 2 : 64;\n", "    int64_t cap = s->cap ? s->cap * 2 : 64;\n    GSQ(grows); if (cap > gsq_maxcap) gsq_maxcap = cap;\n"),
    ("    gs_qlock(&s->mutex);\n    gs_cond_signal(&s->cond);\n", "    GSQ(wakes);\n    gs_qlock(&s->mutex);\n    gs_cond_signal(&s->cond);\n"),
    ("            if (n) return n;\n            for (int r = 0; r < GS_QLOST; r++) gs_pf_relax();\n", "            if (n) return n;\n            GSQ(lost);\n            for (int r = 0; r < GS_QLOST; r++) gs_pf_relax();\n"),
    ("        if (GS_QSEQ_LOAD(&s->tail) == s->head) gs_cond_wait(&s->cond, &s->mutex);\n", "        if (GS_QSEQ_LOAD(&s->tail) == s->head) { GSQ(sleeps); gs_cond_wait(&s->cond, &s->mutex); }\n"),
    ("                free(s->ring);\n                s->ring = NULL;\n                s->cap = 0;\n            }\n", "                free(s->ring);\n                s->ring = NULL;\n                s->cap = 0;\n                GSQ(frees);\n            }\n"),
    ("GS_API void gs_out_end(void) { gs_out_final(); }\n", """GS_API void gs_out_end(void) {
    gs_out_final();
    fprintf(stderr, "grows %lld maxcap %lld frees %lld wakes %lld sleeps %lld lost %lld\\n",
            gsq_grows, gsq_maxcap, gsq_frees, gsq_wakes, gsq_sleeps, gsq_lost);
}
"""),
]


def build(name, pairs):
    c = f"{OUT}/rt_{name}.c"
    open(c, "w", encoding="utf-8").write(patched(pairs))
    exe = f"{OUT}/conc_{name}{EXE}"
    r = run([args.clang, *FLAGS, f"-I{os.path.dirname(rt)}", prog, c, "-o", exe, *LIBS])
    if r.returncode:
        sys.exit(r.stderr[-1500:])
    return exe


progs["two locks"] = build("two", [])
progs["counted"] = build("counted", COUNTED)
progs["counted, ring kept"] = build("keep", COUNTED + KEEP)
counters = build("counters", COUNT)

if shutil.which("rustc") and shutil.which("go"):
    rust = f"{OUT}/conc_rust{EXE}"
    r = run(["rustc", "-C", "debuginfo=0", "-C", "opt-level=3", "-C", "lto=fat", "-C", "codegen-units=1", "-o", rust, f"{SRC}/conc.rs"])
    if r.returncode == 0:
        progs["rust"] = rust
    go = f"{OUT}/conc_go{EXE}"
    r = run(["go", "build", "-o", go, "."], cwd=f"{SRC}/go")
    if r.returncode == 0:
        progs["go"] = go

if args.mask and WINDOWS:
    k = ctypes.windll.kernel32
    k.GetCurrentProcess.restype = ctypes.c_void_p
    k.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    assert k.SetProcessAffinityMask(k.GetCurrentProcess(), args.mask)

print(f"{args.rounds} rounds, each build in turn", flush=True)
for b in ("fanout", "pingpong", "spawnwork"):
    times = {n: [] for n in progs}
    for _ in range(args.rounds):
        for n, exe in progs.items():
            r = run([exe, "1", b], timeout=300)
            times[n].append(float(r.stdout.split("\t")[2]))
    print(f"\n{b}", flush=True)
    for n, v in times.items():
        v.sort()
        step = max(1, len(v) // 8)
        print(f"  {n:<20} least {v[0]:.4f} median {v[len(v) // 2]:.4f} most {v[-1]:.4f} | " + " ".join(f"{x:.3f}" for x in v[::step]), flush=True)

print("\nfanout as it is, with counters, by time", flush=True)
rows = []
for _ in range(args.rounds):
    r = run([counters, "1", "fanout"], timeout=300)
    rows.append((float(r.stdout.split("\t")[2]), r.stderr.strip()))
for t, s in sorted(rows):
    print(f"  {t:.4f}  {s}", flush=True)
