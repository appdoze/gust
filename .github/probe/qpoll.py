"""Temporary: how receivers wait for a message, round by round on one machine: polling less often
the longer they have waited, looking at the tail's line only when what they last saw has run out,
spinning longer before they sleep, and trying a lock before Windows's own wait for it.

    python qpoll_probe.py <new gust> [--old <old gust>] [--out dir] [--rounds n] [--clang path]
"""
import argparse
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
        if t.count(old) != 1:
            sys.exit(f"{t.count(old)} matches for {old[:70]!r}")
        t = t.replace(old, new)
    return t


def poll(most):
    return [("""    for (int spins = GS_QSPIN; spins > 0; spins--) {
        if (GS_QANY(s)) {
            gs_qlock(&s->mutex);
            gs_qnode *n = gs_qtake(s);
            gs_mutex_unlock(&s->mutex);
            if (n) return n;
            for (int r = 0; r < GS_QLOST; r++) gs_pf_relax();
        }
        gs_pf_relax();
    }
""", f"""    int wait = 1;
    for (int spins = GS_QSPIN; spins > 0; spins -= wait) {{
        if (GS_QANY(s)) {{
            gs_qlock(&s->mutex);
            gs_qnode *n = gs_qtake(s);
            gs_mutex_unlock(&s->mutex);
            if (n) return n;
            for (int r = 0; r < GS_QLOST; r++) gs_pf_relax();
        }}
        for (int r = 0; r < wait; r++) gs_pf_relax();
        if (wait < {most}) wait *= 2;
    }}
""")]


SEEN = [("#define GS_QANY(s) (GS_QLOOK(&(s)->tail) != GS_QLOOK(&(s)->head))\n",
         "#define GS_QANY(s) (GS_QLOOK(&(s)->seen_tail) != GS_QLOOK(&(s)->head) || GS_QLOOK(&(s)->tail) != GS_QLOOK(&(s)->head))\n")]
SPIN = [("#define GS_QSPIN 2000\n", "#define GS_QSPIN 8000\n")]
TRY = [("""    #ifndef _WIN32
        for (int i = 0; i < GS_QLOCK_SPIN; i++) {
            if (!pthread_mutex_trylock(m)) return;
            /* Longer each time, as each try takes the lock's line from the
               others, the holder among them. */
            for (int r = 0; r <= i && r < 16; r++) gs_pf_relax();
        }
    #endif
""", """    for (int i = 0; i < GS_QLOCK_SPIN; i++) {
        #ifdef _WIN32
            if (TryAcquireSRWLockExclusive(m)) return;
        #else
            if (!pthread_mutex_trylock(m)) return;
        #endif
        for (int r = 0; r <= i && r < 16; r++) gs_pf_relax();
    }
""")]
LOST = [("#define GS_QLOST 1024\n", "#define GS_QLOST 128\n")]


def build(name, pairs):
    c = f"{OUT}/rt_{name}.c"
    open(c, "w", encoding="utf-8").write(patched(pairs))
    exe = f"{OUT}/conc_{name}{EXE}"
    r = run([args.clang, *FLAGS, f"-I{os.path.dirname(rt)}", prog, c, "-o", exe, *LIBS])
    if r.returncode:
        sys.exit(r.stderr[-1500:])
    return exe


progs["two locks"] = build("two", [])
progs["poll to 16"] = build("poll16", poll(16))
progs["poll to 64"] = build("poll64", poll(64))
progs["seen tail"] = build("seen", SEEN)
progs["spin 8000"] = build("spin", SPIN)
progs["lost 128"] = build("lost", LOST)
if WINDOWS:
    progs["try first"] = build("try", TRY)
    progs["poll64+seen+try"] = build("all", poll(64) + SEEN + TRY)
else:
    progs["poll64+seen"] = build("all", poll(64) + SEEN)

if shutil.which("rustc") and shutil.which("go"):
    rust = f"{OUT}/conc_rust{EXE}"
    r = run(["rustc", "-C", "debuginfo=0", "-C", "opt-level=3", "-C", "lto=fat", "-C", "codegen-units=1", "-o", rust, f"{SRC}/conc.rs"])
    if r.returncode == 0:
        progs["rust"] = rust
    go = f"{OUT}/conc_go{EXE}"
    r = run(["go", "build", "-o", go, "."], cwd=f"{SRC}/go")
    if r.returncode == 0:
        progs["go"] = go

print(f"{args.rounds} rounds, each build in turn", flush=True)
for b in ("fanout", "pingpong", "spawnwork"):
    times = {n: [] for n in progs}
    for _ in range(args.rounds):
        for n, exe in progs.items():
            r = run([exe, "1", b], timeout=300)
            times[n].append(float(r.stdout.split("\t")[2]))
    print(f"\n{b}", flush=True)
    two = sorted(times["two locks"])[args.rounds // 2]
    for n, v in times.items():
        v.sort()
        mid = v[len(v) // 2]
        print(f"  {n:<16} least {v[0]:.4f} median {mid:.4f} most {v[-1]:.4f}  {mid / two:5.2f}x of two locks", flush=True)
