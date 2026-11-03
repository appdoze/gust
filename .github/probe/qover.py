"""Temporary: a queue with more threads than CPUs. Each strategy round by round on one machine,
every Gust build compiled the same way, with what each run cost the CPUs and how often it switched.

    python qover_probe.py <gust> [--out dir] [--rounds n] [--check header]
"""
import argparse
import glob
import os
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
ap.add_argument("--check", help="only see that every patch applies to this header")
args = ap.parse_args()

SPIN_OLD = """    for (int spins = gs_spin_for(GS_QSPIN); spins > 0; spins--) {
        if (gs_qany(s)) {
            gs_qnode *n = gs_qtake_now(s);
            if (n) return n;
            for (int r = 0; r < GS_QLOST; r++) gs_pf_relax();
        }
        gs_pf_relax();
    }
"""
GATE_OLD = """static void gs_qenter(gs_qsmall *l) {
    int looks = gs_spin_for(64);
    for (int i = 0; ; i++) {
        if (!__atomic_load_n(l, __ATOMIC_RELAXED) && !__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) return;
        if (i < looks) {
            for (int r = 0; r <= i && r < 16; r++) gs_pf_relax();
        } else {
            sched_yield();
        }
    }
}
#define gs_qleave(l) __atomic_store_n((l), 0, __ATOMIC_RELEASE)
"""
FIELD_OLD = "    int64_t signalled;             /* of those, how many a sender has signalled; under mutex */\n"
WAKE_OLD = "    if (GS_QSEQ_LOAD(&s->sleeping) > gs_qload(&s->signalled)) gs_qwake(s);\n"

# Spin a little, then give the CPU away a few times before sleeping.
YIELD = [(SPIN_OLD, SPIN_OLD.replace("gs_spin_for(GS_QSPIN)", "gs_spin_for(256)") + """    for (int y = gs_spin_for(32); y > 0; y--) {
        if (gs_qany(s)) {
            gs_qnode *n = gs_qtake_now(s);
            if (n) return n;
        }
        sched_yield();
    }
""")]
# A small lock whose waiter goes off the CPU for a time, rather than yield.
NAP = [(GATE_OLD, GATE_OLD.replace("            sched_yield();\n", "            struct timespec ms = { 0, 1000 };\n            nanosleep(&ms, NULL);\n"))]
# A small lock whose waiter sleeps until it is woken.
FUTEX = [(GATE_OLD, """static void gs_qenter(gs_qsmall *l) {
    int looks = gs_spin_for(64);
    for (int i = 0; i < looks; i++) {
        int32_t none = 0;
        if (!__atomic_load_n(l, __ATOMIC_RELAXED) && __atomic_compare_exchange_n(l, &none, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        for (int r = 0; r <= i && r < 16; r++) gs_pf_relax();
    }
    while (__atomic_exchange_n(l, 2, __ATOMIC_ACQUIRE)) syscall(SYS_futex, l, 128, 2, NULL, NULL, 0);
}
static void gs_qleave(gs_qsmall *l) {
    if (__atomic_exchange_n(l, 0, __ATOMIC_RELEASE) == 2) syscall(SYS_futex, l, 129, 1, NULL, NULL, 0);
}
""")]
# Barely spin while more threads are at work than there are CPUs.
THREADS = [
    ("static int64_t gs_cpus_seen;\n", "static int64_t gs_cpus_seen;\nstatic int64_t gsq_live = 1;\n"),
    ("    return cpus > 1 ? looks : 1;\n", "    if (cpus <= 1) return 1;\n    return __atomic_load_n(&gsq_live, __ATOMIC_RELAXED) > cpus ? (looks >> 5) + 1 : looks;\n"),
    ("        gs_regions_begin(gs_workerregions);\n        gs_current_thread_id = t->id;\n",
     "        __atomic_add_fetch(&gsq_live, 1, __ATOMIC_RELAXED);\n        gs_regions_begin(gs_workerregions);\n        gs_current_thread_id = t->id;\n"),
    ("        gs_mutex_lock(&gs_threads_mutex);\n        gs_thread **slot = &gs_threads;\n",
     "        __atomic_sub_fetch(&gsq_live, 1, __ATOMIC_RELAXED);\n        gs_mutex_lock(&gs_threads_mutex);\n        gs_thread **slot = &gs_threads;\n"),
]
# How long a queue's receivers spin follows whether it has been paying off.
ADAPT = [
    (FIELD_OLD, FIELD_OLD + "    int64_t spin_for;\n"),
    (SPIN_OLD, """    static GS_TLS unsigned gsq_tick;
    int full = gs_spin_for(GS_QSPIN);
    int limit = (int)GS_SPIN_LOAD64(&s->spin_for);
    if (!limit || !(++gsq_tick & 31)) limit = full;
    for (int spins = limit; spins > 0; spins--) {
        if (gs_qany(s)) {
            gs_qnode *n = gs_qtake_now(s);
            if (n) {
                if (limit < full) GS_SPIN_STORE64(&s->spin_for, limit * 2 < full ? limit * 2 : full);
                return n;
            }
            for (int r = 0; r < GS_QLOST; r++) gs_pf_relax();
        }
        gs_pf_relax();
    }
    {
        int less = limit / 2 < 32 ? 32 : limit / 2;
        if (less != GS_SPIN_LOAD64(&s->spin_for)) GS_SPIN_STORE64(&s->spin_for, less);
    }
"""),
]
# No wake while a receiver is spinning, as it will see the message: Go's way with its Ps.
SPINNERS = [
    (FIELD_OLD, FIELD_OLD + "    int64_t spinning;\n"),
    (WAKE_OLD, "    if (GS_QSEQ_LOAD(&s->sleeping) > gs_qload(&s->signalled) && !GS_QSEQ_LOAD(&s->spinning)) gs_qwake(s);\n", "many"),
    (SPIN_OLD, """    int counted = 0;
    int limit = gs_spin_for(GS_QSPIN);
    for (int spins = limit; spins > 0; spins--) {
        if (gs_qany(s)) {
            gs_qnode *n = gs_qtake_now(s);
            if (n) {
                if (counted) {
                    GS_QSEQ_ADD(&s->spinning, -1);
                    if (gs_qany(s) && GS_QSEQ_LOAD(&s->sleeping) > gs_qload(&s->signalled)) gs_qwake(s);
                }
                return n;
            }
            for (int r = 0; r < GS_QLOST; r++) gs_pf_relax();
        }
        gs_pf_relax();
        if (!counted && spins < limit - 16) {
            counted = 1;
            GS_QSEQ_ADD(&s->spinning, 1);
        }
    }
    if (counted) GS_QSEQ_ADD(&s->spinning, -1);
"""),
]
VARIANTS = {
    "main": [],
    "yield": YIELD,
    "nap": NAP,
    "futex": FUTEX,
    "threads": THREADS,
    "adapt": ADAPT,
    "spinners": SPINNERS,
    "futex+threads": FUTEX + THREADS,
    "futex+yield": FUTEX + YIELD,
    "futex+spinners": FUTEX + SPINNERS,
}


def apply(text, pairs):
    for old, new, *many in pairs:
        n = text.count(old)
        if n < 1 or (n != 1 and not many):
            sys.exit(f"{n} matches for {old[:70]!r}")
        text = text.replace(old, new)
    return text


if args.check:
    head = open(args.check, encoding="utf-8").read().replace("\r\n", "\n")
    for name, pairs in VARIANTS.items():
        apply(head, pairs)
        print("applies:", name)
    sys.exit(0)

OUT = os.path.abspath(args.out)
os.makedirs(OUT, exist_ok=True)
HOME = os.path.expanduser("~")
SRC = "compare/concurrency"
FLAGS = "-O2 -ffunction-sections -fdata-sections -fno-stack-protector -w".split()


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


r = run([os.path.abspath(args.gust), "build", "-q", "--cc", "clang", f"{SRC}/conc.goose", "-o", f"{OUT}/conc_gust"])
assert r.returncode == 0, r.stdout + r.stderr
prog = max(glob.glob(f"{HOME}/.gust/build/prog/conc-*/conc.c"), key=os.path.getmtime)
rt = max(glob.glob(f"{HOME}/.gust/build/runtime/*/clang*release*/goose_runtime.c"), key=os.path.getmtime)
base = open(rt, encoding="utf-8", errors="replace").read().replace("\r\n", "\n")

progs = {}
for name, pairs in VARIANTS.items():
    stem = name.replace("+", "_")
    c = f"{OUT}/rt_{stem}.c"
    open(c, "w", encoding="utf-8").write(apply(base, pairs))
    exe = f"{OUT}/conc_{stem}"
    r = run([args.clang, *FLAGS, f"-I{os.path.dirname(rt)}", prog, c, "-o", exe, "-lpthread", "-lm"])
    if r.returncode:
        sys.exit(f"{name}: {r.stderr[-1500:]}")
    progs[name] = exe
if shutil.which("rustc") and shutil.which("go"):
    rust = f"{OUT}/conc_rust"
    r = run(["rustc", "-C", "debuginfo=0", "-C", "opt-level=3", "-C", "lto=fat", "-C", "codegen-units=1", "-o", rust, f"{SRC}/conc.rs"])
    if r.returncode == 0:
        progs["rust"] = rust
    go = f"{OUT}/conc_go"
    r = run(["go", "build", "-o", go, "."], cwd=f"{SRC}/go")
    if r.returncode == 0:
        progs["go"] = go


def once(exe, bench, cpus):
    argv = ["taskset", "-c", cpus, exe, "1", bench] if cpus else [exe, "1", bench]
    p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    out = p.stdout.read()
    _, _, ru = os.wait4(p.pid, 0)
    p.returncode = 0
    return float(out.split("\t")[2]), ru.ru_utime + ru.ru_stime, ru.ru_nvcsw, ru.ru_nivcsw


def rounds(label, bench, cpus):
    rows = {n: [] for n in progs}
    for _ in range(args.rounds):
        for n, exe in progs.items():
            rows[n].append(once(exe, bench, cpus))
    print(f"\n{label}: {bench}", flush=True)
    ref = statistics.median(r[0] for r in rows["main"])
    for n, rs in rows.items():
        wall = sorted(r[0] for r in rs)
        print(f"  {n:<15} {statistics.median(wall):.4f} ({wall[0]:.4f}-{wall[-1]:.4f}) {statistics.median(wall) / ref:5.2f}x"
              f" | cpu {statistics.median(r[1] for r in rs):.3f}s"
              f" | switches {int(statistics.median(r[2] for r in rs))} asked {int(statistics.median(r[3] for r in rs))} forced", flush=True)


print(f"{os.cpu_count()} CPUs; {args.rounds} rounds, each build in turn; medians", flush=True)
print(run("lscpu | grep -E 'Model name'; uname -r", shell=True).stdout.strip(), flush=True)
start = time.time()
for label, cpus in (("all CPUs", ""), ("two CPUs, two cores", "0,2"), ("two CPUs, one core", "0,1"), ("one CPU", "0")):
    rounds(label, "fanout", cpus)
for label, cpus in (("all CPUs", ""), ("two CPUs, two cores", "0,2")):
    rounds(label, "pingpong", cpus)
print(f"\n{time.time() - start:.0f} s", flush=True)
