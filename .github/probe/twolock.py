"""Temporary: the two-lock queue under ThreadSanitizer, and against the one-lock queue,
Rust and Go on one machine."""
import glob, os, statistics, subprocess, sys

HOME = os.path.expanduser("~")
NEW = os.path.abspath("build/gust")
OLD = "/tmp/old/build/gust"


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


print(run("nproc; lscpu | grep -E 'Model name|Thread|Core'", shell=True).stdout, flush=True)

# ThreadSanitizer: the program's C and the runtime's, as the compiler wrote them.
bad = 0
for t in ("queue_stress", "queue_ring", "queue_bounds", "queue_producers", "mailboxes", "thread_reuse"):
    src = f"test/threads/{t}.goose"
    r = run([NEW, "build", "-q", "--cc", "clang", src, "-o", f"/tmp/{t}_plain"])
    assert r.returncode == 0, r.stdout + r.stderr
    prog = max(glob.glob(f"{HOME}/.gust/build/prog/{t}-*/{t}.c"), key=os.path.getmtime)
    rt = max(glob.glob(f"{HOME}/.gust/build/runtime/*/clang*release*/goose_runtime.c"), key=os.path.getmtime)
    exe = f"/tmp/{t}_tsan"
    r = run(["clang", "-fsanitize=thread", "-O1", "-g", "-w", prog, rt, "-o", exe, "-lpthread", "-lm"])
    assert r.returncode == 0, r.stderr[-1000:]
    env = {**os.environ, "TSAN_OPTIONS": "halt_on_error=0 exitcode=66 report_signal_unsafe=0"}
    r = run([exe], env=env, timeout=900)
    races = r.stderr.count("WARNING: ThreadSanitizer")
    want = open(f"test/expected/{t}.out").read()
    print(f"tsan {t}: exit {r.returncode}, reports {races}, output right {r.stdout == want}", flush=True)
    if races or r.returncode or r.stdout != want:
        bad += 1
        print(r.stderr[:6000], flush=True)
print("tsan problems:", bad, flush=True)

# The same benchmark four ways, round by round on one machine.
src = "compare/concurrency"
progs = {}
for name, gust in (("two locks", NEW), ("one lock", OLD)):
    exe = f"/tmp/conc_{name.replace(' ', '_')}"
    r = run([gust, "build", "-q", "--cc", "clang", f"{src}/conc.goose", "-o", exe])
    assert r.returncode == 0, r.stdout + r.stderr
    progs[name] = exe
r = run(["rustc", "-C", "debuginfo=0", "-C", "opt-level=3", "-C", "lto=fat", "-C", "codegen-units=1", "-o", "/tmp/conc_rust", f"{src}/conc.rs"])
assert r.returncode == 0, r.stderr[-500:]
progs["rust"] = "/tmp/conc_rust"
r = run(["go", "build", "-o", "/tmp/conc_go", "."], cwd=f"{src}/go")
assert r.returncode == 0, r.stderr[-500:]
progs["go"] = "/tmp/conc_go"

print(f"\n{'bench':<11}" + "".join(f"{n:>11}" for n in progs) + "  two/one  vs rust  vs go", flush=True)
for b in ("spawnjoin", "spawnmany", "spawnwork", "pingpong", "fanout"):
    best = {}
    for _ in range(9):
        for n, exe in progs.items():
            v = float(run([exe, "1", b]).stdout.split("\t")[2])
            best[n] = min(best.get(n, 1e9), v)
    print(f"{b:<11}" + "".join(f"{best[n]:11.4f}" for n in progs) +
          f"  {best['two locks'] / best['one lock']:6.2f}x {best['two locks'] / best['rust']:7.2f}x {best['two locks'] / best['go']:6.2f}x", flush=True)
sys.exit(1 if bad else 0)
