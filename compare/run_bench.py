"""Build and run the cross-language benchmarks: Gust, Rust and, where it is
installed, Lobster. Writes results.json beside this script (or --out).

    python compare/run_bench.py [--runs N] [--repeats N] [--only config,config] [--out path]
    python compare/run_bench.py --check

Benchmarks 1-5 follow SPEC.md (Lobster's suite), 6-8 EXTRA_SPEC.md. Every
program times each benchmark itself and prints its best of `repeats`; this
script launches each configuration `runs` times, round-robin, and keeps the
best of those, checks that every configuration agrees on every checksum,
measures each benchmark's peak memory in a process of its own, and records
build times and binary sizes.

--check is what CI runs: each configuration that can be built here is
built and run once, one repeat, and the run fails if any Gust configuration
does not build or run, or if any two configurations disagree on a checksum.
Timings from it mean nothing.

Configurations are skipped where their tools are missing: rustc for Rust,
LOBSTER (lobster.exe, or its repository root) for Lobster, MSVC off
Windows, gcc off Linux. The Lobster C++ builds also need LOBSTER_MSBUILD and
LOBSTER_VCVARS on Windows, as the shipped Lobster project builds with them.
"""

import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(HERE, "build")
EXE = ".exe" if os.name == "nt" else ""


def scrub(text):
    """text with this machine's home directory and the repository's path
    replaced, so that a recorded result names no local paths."""
    for path, name in ((ROOT, "<repo>"), (os.path.expanduser("~"), "~")):
        for form in {path, path.replace("\\", "/"), path.replace("/", "\\")}:
            text = text.replace(form, name)
    return text


def scrub_all(v):
    """scrub over every string in a result."""
    if isinstance(v, str):
        return scrub(v)
    if isinstance(v, dict):
        return {k: scrub_all(x) for k, x in v.items()}
    if isinstance(v, list):
        return [scrub_all(x) for x in v]
    return v
BENCHES = ["nbody", "raytrace", "interp", "sort", "astar", "bintrees", "hashmap", "strings"]
GUST_MAIN = os.path.join(HERE, "gust", "main.goose")


def find_gust():
    env = os.environ.get("GUST")
    if env:
        # A path is made absolute here: the builds run in other directories,
        # where a relative one would name nothing (POSIX resolves a program's
        # path against the directory it starts in).
        return os.path.abspath(env) if "/" in env or os.sep in env else env
    for d in ("build/Release", "build/RelWithDebInfo", "build", "build/Debug"):
        p = os.path.join(ROOT, d, "gust" + EXE)
        if os.path.exists(p):
            return p
    sys.exit("no gust under build/; set GUST")


def find_lobster():
    """lobster.exe and its repository root, from LOBSTER; (None, None) without it."""
    env = os.environ.get("LOBSTER")
    if not env:
        return None, None
    if os.path.isdir(env):
        exe = os.path.join(env, "bin", "lobster" + EXE)
        return (exe, env) if os.path.exists(exe) else (None, None)
    root = os.path.dirname(os.path.dirname(os.path.abspath(env)))
    return (env, root) if os.path.exists(env) else (None, None)


GUST = find_gust()
LOBSTER, LOBSTER_ROOT = find_lobster()


# --- process memory --------------------------------------------------------

if os.name == "nt":
    import ctypes
    import ctypes.wintypes as wt

    class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
        _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD),
                    ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]

    _psapi = ctypes.WinDLL("psapi")
    _psapi.GetProcessMemoryInfo.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESS_MEMORY_COUNTERS), wt.DWORD]


def run_measured(cmd, cwd):
    """Runs cmd to completion: (exit code, output, wall seconds, peak resident
    bytes or None). Windows reads the peak working set through the process
    handle, which stays open after the exit; POSIX takes ru_maxrss from wait4."""
    t0 = time.perf_counter()
    if os.name == "nt":
        # High priority, so a desktop's background load disturbs the runs less.
        p = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                             creationflags=subprocess.HIGH_PRIORITY_CLASS)
        out, _ = p.communicate(timeout=3600)
        wall = time.perf_counter() - t0
        pmc = PROCESS_MEMORY_COUNTERS()
        pmc.cb = ctypes.sizeof(pmc)
        ok = _psapi.GetProcessMemoryInfo(wt.HANDLE(int(p._handle)), ctypes.byref(pmc), pmc.cb)
        return p.returncode, out, wall, pmc.PeakWorkingSetSize if ok else None
    p = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    out = p.stdout.read()
    _, status, usage = os.wait4(p.pid, 0)
    p.returncode = os.waitstatus_to_exitcode(status)
    wall = time.perf_counter() - t0
    # Linux reports KB, macOS bytes.
    peak = usage.ru_maxrss * (1 if sys.platform == "darwin" else 1024)
    return p.returncode, out, wall, peak


def sh(cmd, cwd=HERE, env=None):
    t0 = time.perf_counter()
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, env=env)
    return r, time.perf_counter() - t0


# --- configurations --------------------------------------------------------

def build_rust(name, flags):
    def go():
        out = os.path.join(BUILD, name + EXE)
        r, t = sh(["rustc", "-C", "debuginfo=0", *flags, "-o", out, os.path.join(HERE, "rust", "main.rs")])
        if r.returncode != 0:
            raise RuntimeError(r.stderr[-500:])
        return t, out
    return go


def build_gust(name, cc, extra=()):
    def go():
        out = os.path.join(BUILD, name + EXE)
        r, t = sh([GUST, "build", "-q", "--cc", cc, *extra, GUST_MAIN, "-o", out])
        if r.returncode != 0:
            raise RuntimeError((r.stderr or r.stdout)[-500:])
        return t, out
    return go


def build_lobster_cpp():
    """lobster --cpp, then the shipped Visual Studio project (Windows)."""
    msbuild = os.environ.get("LOBSTER_MSBUILD")
    sln = os.path.join(LOBSTER_ROOT, "dev", "compiled_lobster", "compiled_lobster", "compiled_lobster.sln")
    built = os.path.join(LOBSTER_ROOT, "dev", "compiled_lobster", "compiled_lobster", "x64", "Release",
                         "compiled_lobster.exe")
    if not msbuild or not os.path.exists(msbuild):
        raise RuntimeError("skipped: LOBSTER_MSBUILD is not set")
    r1, t1 = sh([LOBSTER, "--cpp", os.path.join(HERE, "lobster", "main.lobster")], cwd=LOBSTER_ROOT)
    if r1.returncode != 0:
        raise RuntimeError((r1.stderr or r1.stdout)[-500:])
    r2, t2 = sh([msbuild, sln, "-p:Configuration=Release", "-p:Platform=x64", "-m", "-v:m", "-nologo"],
                cwd=LOBSTER_ROOT)
    if r2.returncode != 0:
        raise RuntimeError((r2.stdout or r2.stderr)[-500:])
    out = os.path.join(BUILD, "lobster-cpp-msvc.exe")
    shutil.copy2(built, out)
    for d in ("steam_api64.dll", "openvr_api.dll"):
        src = os.path.join(LOBSTER_ROOT, "bin", d)
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(BUILD, d))
    return t1 + t2, out


def configs():
    """The configurations this machine can run: (name, lang, build or None, cmd, cwd, note)."""
    cs = []
    win, linux = os.name == "nt", sys.platform.startswith("linux")
    cs.append(dict(name="gust/clang", lang="gust", build=build_gust("gust-clang", "clang"),
                   note="gust build --cc clang: Gust -O2 to C, then clang -O2"))
    cs.append(dict(name="gust/clang-checked", lang="gust",
                   build=build_gust("gust-clang-checked", "clang", ["--checked"]),
                   note="gust build --checked --cc clang: the same with overflow, range and tag checks"))
    if win:
        cs.append(dict(name="gust/msvc", lang="gust", build=build_gust("gust-msvc", "msvc"),
                       note="gust build --cc msvc: Gust -O2 to C, then cl /O2"))
    if linux:
        cs.append(dict(name="gust/gcc", lang="gust", build=build_gust("gust-gcc", "gcc"),
                       note="gust build --cc gcc: Gust -O2 to C, then gcc -O2"))
    cs.append(dict(name="gust/tcc", lang="gust", cmd=[GUST, "run", "-q", GUST_MAIN, "--"],
                   note="gust run: Gust to C compiled in-process by TinyCC (no build step)"))
    if shutil.which("rustc"):
        cs.append(dict(name="rust/o3", lang="rust", build=build_rust("rust-o3", ["-C", "opt-level=3"]),
                       note="rustc -C opt-level=3"))
        cs.append(dict(name="rust/o3-lto", lang="rust",
                       build=build_rust("rust-o3-lto", ["-C", "opt-level=3", "-C", "lto=fat",
                                                        "-C", "codegen-units=1"]),
                       note="rustc -C opt-level=3 -C lto=fat -C codegen-units=1"))
    if LOBSTER:
        main = os.path.join(HERE, "lobster", "main.lobster")
        cs.append(dict(name="lobster/libtcc", lang="lobster", cmd=[LOBSTER, main, "--"], cwd=LOBSTER_ROOT,
                       note="lobster (default): C compiled in-process by libtcc"))
        cs.append(dict(name="lobster/mir2", lang="lobster", cmd=[LOBSTER, "--mir", "2", main, "--"],
                       cwd=LOBSTER_ROOT, note="lobster --mir 2: MIR optimising JIT"))
        if win:
            cs.append(dict(name="lobster/cpp-msvc", lang="lobster", build=build_lobster_cpp,
                           note="lobster --cpp, then MSVC /O2 (the shipped project)"))
    return cs


def parse(text):
    times, sums = {}, {}
    for line in text.splitlines():
        parts = line.strip().split("\t")
        if len(parts) == 4 and parts[0] == "bench":
            times[parts[1]] = float(parts[2])
            sums[parts[1]] = float(parts[3])
    return times, sums


# --- source size -----------------------------------------------------------

SECTION = re.compile(r"^// -{10,} (?:\d+\. )?(\w+)", re.M)


def code_lines(text, comment):
    return sum(1 for line in text.splitlines() if line.strip() and not line.strip().startswith(comment))


def sections(path):
    """A one-file implementation split at its `// ---- N. name` markers."""
    text = open(path, encoding="utf-8").read()
    marks = [(m.start(), m.group(1)) for m in SECTION.finditer(text)]
    return {name: text[pos:marks[i + 1][0] if i + 1 < len(marks) else len(text)]
            for i, (pos, name) in enumerate(marks)}


def source_sizes():
    sizes = {}
    for lang, path in (("rust", "rust/main.rs"), ("gust", "gust/main.goose")):
        sizes[lang] = {name: {"lines": code_lines(body, "//"), "bytes": len(body.encode())}
                       for name, body in sections(os.path.join(HERE, path)).items()}
    files = {"shared": "rnd.lobster", "nbody": "nbody.lobster", "raytrace": "raytrace.lobster",
             "interp": "interp.lobster", "sort": "sortbench.lobster", "astar": "astar.lobster",
             "bintrees": "bintrees.lobster", "hashmap": "hashmap.lobster", "strings": "strings.lobster",
             "driver": "main.lobster"}
    sizes["lobster"] = {}
    for name, f in files.items():
        body = open(os.path.join(HERE, "lobster", f), encoding="utf-8").read()
        sizes["lobster"][name] = {"lines": code_lines(body, "//"), "bytes": len(body.encode())}
    return sizes


def machine():
    cpu = platform.processor() or platform.machine()
    if os.name == "nt":
        try:
            r = subprocess.run(["powershell", "-NoProfile", "-Command",
                                "(Get-CimInstance Win32_Processor).Name; "
                                "[math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory/1GB)"],
                               capture_output=True, text=True)
            lines = [l.strip() for l in r.stdout.splitlines() if l.strip()]
            if lines:
                cpu = f"{lines[0]}, {lines[1]} GB RAM" if len(lines) > 1 else lines[0]
        except OSError:
            pass
    return f"{cpu}; {platform.platform()}"


def tool_versions():
    v = {}
    r = subprocess.run([GUST, "version"], capture_output=True, text=True)
    v["gust"] = r.stdout.strip()
    if shutil.which("rustc"):
        v["rustc"] = subprocess.run(["rustc", "--version"], capture_output=True, text=True).stdout.strip()
    r = subprocess.run([GUST, "toolchain"], capture_output=True, text=True)
    v["toolchain"] = r.stdout.strip()
    return v


# --- main ------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--repeats", type=int, default=5)
    ap.add_argument("--only", default="")
    ap.add_argument("--out", default=os.path.join(HERE, "results.json"))
    ap.add_argument("--check", action="store_true",
                    help="build and run each configuration once and check the checksums agree (CI)")
    args = ap.parse_args()
    if args.check:
        args.runs, args.repeats = 1, 1
    os.makedirs(BUILD, exist_ok=True)
    only = set(filter(None, args.only.split(",")))
    results = {"machine": machine(), "runs": args.runs, "repeats": args.repeats, "tools": tool_versions(),
               "generated": time.strftime("%Y-%m-%d %H:%M"), "configs": []}
    active = []
    for cfg in configs():
        if only and cfg["name"] not in only:
            continue
        rec = {"name": cfg["name"], "lang": cfg["lang"], "note": cfg["note"], "status": "ok",
               "seconds": {}, "checksums": {}}
        results["configs"].append(rec)
        print(f"== build {cfg['name']}", flush=True)
        try:
            if cfg.get("build"):
                secs, exe = cfg["build"]()
                rec["build_seconds"] = secs
                rec["binary_bytes"] = os.path.getsize(exe)
                cmd = [exe]
                print(f"   built in {secs:.1f}s, {rec['binary_bytes'] / 1e6:.2f} MB", flush=True)
            else:
                cmd = cfg["cmd"]
            active.append((rec, cmd, cfg.get("cwd", HERE)))
        except Exception as e:  # a configuration that cannot build is reported, not fatal
            rec["status"] = f"build failed: {e}"
            print(f"   {rec['status']}", flush=True)

    def guarded(rec, fn):
        if rec["status"] != "ok":
            return
        try:
            fn()
        except Exception as e:
            rec["status"] = f"run failed: {e}"
            print(f"   {rec['name']}: {rec['status']}", flush=True)

    if not args.check:
        # Startup: a launch that runs no benchmark; for a JIT, the compile.
        for rec, cmd, cwd in active:
            def startup():
                code, out, wall, peak = run_measured(cmd + ["1", "none"], cwd)
                rec["startup_seconds"], rec["startup_peak_bytes"] = wall, peak
            guarded(rec, startup)

    # Round-robin over the configurations, so that a slow patch of background
    # load lands on all of them rather than on whichever was running.
    for i in range(args.runs):
        for rec, cmd, cwd in active:
            def timed():
                code, out, wall, peak = run_measured(cmd + [str(args.repeats)], cwd)
                if code != 0:
                    raise RuntimeError(f"exit {code}: {out[-500:]}")
                times, sums = parse(out)
                if set(times) != set(BENCHES):
                    raise RuntimeError(f"ran {sorted(times)}, not every benchmark: {out[-300:]}")
                for k, v in times.items():
                    rec["seconds"][k] = min(rec["seconds"].get(k, v), v)
                rec["checksums"] = sums
                print(f"   run {i + 1} {rec['name']}: " + " ".join(f"{k}={v:.3f}" for k, v in times.items()),
                      flush=True)
            guarded(rec, timed)

    if not args.check:
        for rec, cmd, cwd in active:
            def memory():
                mem = {}
                for b in BENCHES:
                    code, out, wall, peak = run_measured(cmd + ["1", b], cwd)
                    mem[b] = peak
                rec["peak_bytes"] = mem
                print(f"   peak MB {rec['name']}: " +
                      " ".join(f"{k}={(v or 0) / 1e6:.1f}" for k, v in mem.items()), flush=True)
            guarded(rec, memory)

    # Every configuration must agree with the first on every checksum (relative 1e-9).
    problems = []
    for b in BENCHES:
        ref = next((c for c in results["configs"] if c["status"] == "ok" and b in c["checksums"]), None)
        if not ref:
            continue
        for c in results["configs"]:
            got = c["checksums"].get(b)
            if got is None:
                continue
            want = ref["checksums"][b]
            if abs(got - want) / max(abs(want), 1e-12) > 1e-9:
                problems.append(f"{b}: {c['name']} gave {got!r}, {ref['name']} gave {want!r}")
    results["checksum_problems"] = problems
    results["source"] = source_sizes()
    if not args.check:
        with open(args.out, "w") as f:
            json.dump(scrub_all(results), f, indent=2)
        print("wrote", args.out)
    print("checksum problems:", problems or "none")
    failed = [c["name"] for c in results["configs"]
              if c["lang"] == "gust" and c["status"] != "ok"]
    agreeing = [c["name"] for c in results["configs"] if c["status"] == "ok"]
    if args.check:
        print("configurations that ran:", ", ".join(agreeing))
        skipped = [c["name"] + " (" + c["status"] + ")" for c in results["configs"] if c["status"] != "ok"]
        if skipped:
            print("did not run:", "; ".join(skipped))
    if problems or failed or len(agreeing) < 2:
        if failed:
            print("FAIL: Gust configuration(s) did not build or run:", ", ".join(failed))
        if len(agreeing) < 2:
            print("FAIL: fewer than two configurations to compare")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
