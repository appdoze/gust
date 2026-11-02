"""Temporary: the two-lock queue against the one-lock queue on Windows, on one machine, with
Rust and Go. The earlier probes were all on Linux."""
import os
import subprocess
import sys

TEMP = os.environ.get("RUNNER_TEMP", ".")
NEW = os.path.abspath("build/Release/gust.exe")
OLD = os.path.join(TEMP, "old", "build", "Release", "gust.exe")
SRC = "compare/concurrency"
ROUNDS = 25


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


print(run("wmic cpu get Name,NumberOfCores,NumberOfLogicalProcessors", shell=True).stdout.strip(), flush=True)

progs = {}
for name, gust, cc in (
    ("two/clang", NEW, "clang"),
    ("one/clang", OLD, "clang"),
    ("two/msvc", NEW, "msvc"),
    ("one/msvc", OLD, "msvc"),
):
    exe = os.path.join(TEMP, f"conc_{name.replace('/', '_')}.exe")
    r = run([gust, "build", "-q", "--cc", cc, f"{SRC}/conc.goose", "-o", exe])
    assert r.returncode == 0, r.stdout + r.stderr
    progs[name] = exe
rust = os.path.join(TEMP, "conc_rust.exe")
r = run(["rustc", "-C", "debuginfo=0", "-C", "opt-level=3", "-C", "lto=fat", "-C", "codegen-units=1", "-o", rust, f"{SRC}/conc.rs"])
assert r.returncode == 0, r.stderr[-500:]
progs["rust"] = rust
go = os.path.join(TEMP, "conc_go.exe")
r = run(["go", "build", "-o", go, "."], cwd=f"{SRC}/go")
assert r.returncode == 0, r.stderr[-500:]
progs["go"] = go

print(f"\nleast and median of {ROUNDS} rounds, each build in turn", flush=True)
print(f"{'bench':<11}" + "".join(f"{n:>19}" for n in progs) + "  two/one clang  msvc", flush=True)
for b in ("spawnwork", "pingpong", "fanout"):
    times = {n: [] for n in progs}
    for _ in range(ROUNDS):
        for n, exe in progs.items():
            times[n].append(float(run([exe, "1", b]).stdout.split("\t")[2]))
    low = {n: min(v) for n, v in times.items()}
    mid = {n: sorted(v)[len(v) // 2] for n, v in times.items()}
    print(f"{b:<11}" + "".join(f"{low[n]:9.4f} {mid[n]:9.4f}" for n in progs) +
          f"  least {low['two/clang'] / low['one/clang']:5.2f}x {low['two/msvc'] / low['one/msvc']:5.2f}x"
          f"  median {mid['two/clang'] / mid['one/clang']:5.2f}x {mid['two/msvc'] / mid['one/msvc']:5.2f}x", flush=True)
    if b == "fanout":
        for n in progs:
            print(f"  {n:<10} " + " ".join(f"{x:.3f}" for x in sorted(times[n])), flush=True)
sys.exit(0)
