"""Temporary: where a queue's head and its lock live, and which lock, round by round on one
machine, every Gust build compiled the same way.

    python qlock_probe.py <gust> [--out dir] [--rounds n] [--clang path]
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys

ap = argparse.ArgumentParser()
ap.add_argument("gust")
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


r = run([os.path.abspath(args.gust), "build", "-q", "--cc", "clang", f"{SRC}/conc.goose", "-o", f"{OUT}/conc_gust{EXE}"])
assert r.returncode == 0, r.stdout + r.stderr
prog = max(glob.glob(f"{HOME}/.gust/build/prog/conc-*/conc.c"), key=os.path.getmtime)
rt = max(glob.glob(f"{HOME}/.gust/build/runtime/*/clang*release*/goose_runtime.c"), key=os.path.getmtime)
base = open(rt, encoding="utf-8", errors="replace").read()


def patched(pairs):
    t = base
    for old, new, *many in pairs:
        n = t.count(old)
        if n < 1 or (n != 1 and not many):
            sys.exit(f"{n} matches for {old[:70]!r}")
        t = t.replace(old, new)
    return t


OLD_STRUCT_HEAD = """struct gs_qstate {
    gs_mutex tail_lock;
    int64_t tail;                  /* written under tail_lock */
    int64_t seen_head;             /* under tail_lock */
    char pad0[GS_QLINE];
    gs_mutex mutex;                /* head's lock, which cond goes with */
    gs_cond cond;
    int64_t head;                  /* written under mutex */
    int64_t seen_tail;             /* written under mutex; gs_qany looks at it outside */
    char pad1[GS_QLINE];
    int64_t limit;
"""
ALIGNED_INIT = ("""    struct gs_qstate *s = (struct gs_qstate *)calloc(1, sizeof(struct gs_qstate));
    if (!s) gs_panic("out of memory creating a queue");
""", """    #ifdef _WIN32
        struct gs_qstate *s = (struct gs_qstate *)_aligned_malloc(sizeof(struct gs_qstate), 64);
    #else
        struct gs_qstate *s = (struct gs_qstate *)aligned_alloc(64, sizeof(struct gs_qstate));
    #endif
    if (!s) gs_panic("out of memory creating a queue");
    memset(s, 0, sizeof(struct gs_qstate));
""")
A = "__attribute__((aligned(64)))"

BESIDE = [
    (OLD_STRUCT_HEAD, f"""struct gs_qstate {{
    gs_mutex tail_lock {A};
    int64_t tail;
    int64_t seen_head;
    gs_mutex mutex {A};
    int64_t head;
    int64_t seen_tail;
    gs_cond cond {A};
    int64_t limit {A};
"""),
    ALIGNED_INIT,
]

GATE_FNS = """
#if defined(_WIN32)
#define GSQ_YIELD() SwitchToThread()
#else
#include <sched.h>
#define GSQ_YIELD() sched_yield()
#endif
static void gs_qgate(int32_t *g) {
    for (int i = 0; ; i++) {
        if (!__atomic_load_n(g, __ATOMIC_RELAXED) && !__atomic_exchange_n(g, 1, __ATOMIC_ACQUIRE)) return;
        if (i < 64) {
            for (int r = 0; r <= i && r < 16; r++) gs_pf_relax();
        } else {
            GSQ_YIELD();
        }
    }
}
#define gs_qungate(g) __atomic_store_n((g), 0, __ATOMIC_RELEASE)
"""

# head with a small lock of its own on its line; mutex and cond only for sleeping and mailboxes.
HEAD_GATE = [
    (OLD_STRUCT_HEAD, f"""struct gs_qstate {{
    gs_mutex tail_lock {A};
    int64_t tail;
    int64_t seen_head;
    int32_t gate {A};
    int64_t head;
    int64_t seen_tail;
    gs_mutex mutex {A};
    gs_cond cond;
    int64_t limit {A};
"""),
    ALIGNED_INIT,
    ("#define GS_QLOCK_SPIN 128\n", "#define GS_QLOCK_SPIN 128\n" + GATE_FNS),
    ("""static void gs_qlock_both(struct gs_qstate *s) {
    gs_qlock(&s->mutex);
""", """static void gs_qlock_both(struct gs_qstate *s) {
    gs_qlock(&s->mutex);
    gs_qgate(&s->gate);
"""),
    ("""    gs_mutex_unlock(&s->tail_lock);
    gs_mutex_unlock(&s->mutex);
}
""", """    gs_mutex_unlock(&s->tail_lock);
    gs_qungate(&s->gate);
    gs_mutex_unlock(&s->mutex);
}
"""),
    ("""        if (gs_qany(s)) {
            gs_qlock(&s->mutex);
            gs_qnode *n = gs_qtake(s);
            gs_mutex_unlock(&s->mutex);
""", """        if (gs_qany(s)) {
            gs_qgate(&s->gate);
            gs_qnode *n = gs_qtake(s);
            gs_qungate(&s->gate);
"""),
    ("""    gs_out_flush();
    gs_qlock(&s->mutex);
    gs_qnode *n = gs_qtake(s);
    while (!n) {
        GS_QSEQ_ADD(&s->sleeping, 1);
        if (GS_QSEQ_LOAD(&s->tail) == s->head) gs_cond_wait(&s->cond, &s->mutex);
        GS_QSEQ_ADD(&s->sleeping, -1);
        n = gs_qtake(s);
    }
""", """    gs_out_flush();
    gs_qlock(&s->mutex);
    gs_qgate(&s->gate);
    gs_qnode *n = gs_qtake(s);
    gs_qungate(&s->gate);
    while (!n) {
        GS_QSEQ_ADD(&s->sleeping, 1);
        if (GS_QSEQ_LOAD(&s->tail) == GS_QLOOK(&s->head)) gs_cond_wait(&s->cond, &s->mutex);
        GS_QSEQ_ADD(&s->sleeping, -1);
        gs_qgate(&s->gate);
        n = gs_qtake(s);
        gs_qungate(&s->gate);
    }
"""),
    ("""    gs_qlock(&s->mutex);
    gs_qnode *n = gs_qtake(s);
    if (!n && ms > 0) {
        gs_mutex_unlock(&s->mutex);
        gs_qspin(s);
        gs_out_flush();
        gs_qlock(&s->mutex);
        n = gs_qtake(s);
        int waiting = 1;
        while (!n && waiting) {
            GS_QSEQ_ADD(&s->sleeping, 1);
            if (GS_QSEQ_LOAD(&s->tail) == s->head)
                waiting = gs_cond_wait_until(&s->cond, &s->mutex, deadline);
            GS_QSEQ_ADD(&s->sleeping, -1);
            n = gs_qtake(s);
        }
    }
    gs_mutex_unlock(&s->mutex);
    return n;
""", """    gs_qgate(&s->gate);
    gs_qnode *n = gs_qtake(s);
    gs_qungate(&s->gate);
    if (!n && ms > 0) {
        gs_qspin(s);
        gs_out_flush();
        gs_qlock(&s->mutex);
        gs_qgate(&s->gate);
        n = gs_qtake(s);
        gs_qungate(&s->gate);
        int waiting = 1;
        while (!n && waiting) {
            GS_QSEQ_ADD(&s->sleeping, 1);
            if (GS_QSEQ_LOAD(&s->tail) == GS_QLOOK(&s->head))
                waiting = gs_cond_wait_until(&s->cond, &s->mutex, deadline);
            GS_QSEQ_ADD(&s->sleeping, -1);
            gs_qgate(&s->gate);
            n = gs_qtake(s);
            gs_qungate(&s->gate);
        }
        gs_mutex_unlock(&s->mutex);
    }
    return n;
"""),
]

# And tail's lock the same.
TAIL_GATE = [
    ("    gs_mutex tail_lock " + A + ";\n", "    int32_t tail_gate " + A + ";\n"),
    ("    gs_mutex_init(&s->tail_lock);\n", ""),
    ("gs_qlock(&s->tail_lock);", "gs_qgate(&s->tail_gate);", "many"),
    ("gs_mutex_unlock(&s->tail_lock);", "gs_qungate(&s->tail_gate);", "many"),
]


def both_gates():
    t = patched(HEAD_GATE)
    for old, new, *many in TAIL_GATE:
        n = t.count(old)
        if n < 1 or (n != 1 and not many):
            sys.exit(f"{n} matches for {old[:70]!r}")
        t = t.replace(old, new)
    return t


def build(name, text):
    c = f"{OUT}/rt_{name}.c"
    open(c, "w", encoding="utf-8").write(text)
    exe = f"{OUT}/conc_{name}{EXE}"
    r = run([args.clang, *FLAGS, f"-I{os.path.dirname(rt)}", prog, c, "-o", exe, *LIBS])
    if r.returncode:
        sys.exit(r.stderr[-1500:])
    return exe


progs = {
    "as it is": build("base", base),
    "head beside lock": build("beside", patched(BESIDE)),
    "head gate": build("gate", patched(HEAD_GATE)),
    "both gates": build("gates", both_gates()),
}

if shutil.which("rustc") and shutil.which("go"):
    rust = f"{OUT}/conc_rust{EXE}"
    r = run(["rustc", "-C", "debuginfo=0", "-C", "opt-level=3", "-C", "lto=fat", "-C", "codegen-units=1", "-o", rust, f"{SRC}/conc.rs"])
    if r.returncode == 0:
        progs["rust"] = rust
    go = f"{OUT}/conc_go{EXE}"
    r = run(["go", "build", "-o", go, "."], cwd=f"{SRC}/go")
    if r.returncode == 0:
        progs["go"] = go


def rounds(bench, prefix=()):
    times = {n: [] for n in progs}
    for _ in range(args.rounds):
        for n, exe in progs.items():
            r = run([*prefix, exe, "1", bench], timeout=300)
            times[n].append(float(r.stdout.split("\t")[2]))
    ref = sorted(times["as it is"])[args.rounds // 2]
    for n, v in times.items():
        v.sort()
        mid = v[len(v) // 2]
        print(f"  {n:<18} least {v[0]:.4f} median {mid:.4f} most {v[-1]:.4f}  {mid / ref:5.2f}x", flush=True)


print(f"{args.rounds} rounds, each build in turn", flush=True)
if not WINDOWS:
    print(run("lscpu | grep -E 'Model name|^CPU\\(s\\)|Thread'", shell=True).stdout, flush=True)
for b in ("fanout", "pingpong", "spawnwork"):
    print(f"\n{b}", flush=True)
    rounds(b)
if not WINDOWS:
    # Fewer CPUs than threads: a lock's holder can be off a CPU with the others spinning.
    for cpus in ("0", "0,1"):
        print(f"\nfanout on CPUs {cpus}", flush=True)
        rounds("fanout", ("taskset", "-c", cpus))
