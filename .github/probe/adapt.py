"""Temporary: what ideas from Zig and Nim would be worth to Gust, measured on Linux.
Each variant patches the runtime's C text and compiles it with the program's C."""
import glob, os, shutil, statistics, subprocess, sys, time, traceback

HOME = os.path.expanduser("~")
GUST = os.path.abspath("build/gust")
OUT = "/tmp/a"
os.makedirs(OUT, exist_ok=True)
ZIG = [sys.executable, "-m", "ziglang", "cc"]
FLAGS = "-O2 -ffunction-sections -fdata-sections -fno-stack-protector -w".split()


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def section(title):
    print(f"\n===== {title}", flush=True)


def gust_c(src, name):
    """The program's C and the runtime's, as the compiler writes them."""
    r = run([GUST, "build", "-q", "--cc", "clang", src, "-o", f"{OUT}/{name}_plain"])
    assert r.returncode == 0, r.stdout + r.stderr
    stem = os.path.basename(src)[:-6]
    prog = max(glob.glob(f"{HOME}/.gust/build/prog/{stem}-*/{stem}.c"), key=os.path.getmtime)
    rt = max(glob.glob(f"{HOME}/.gust/build/runtime/*/clang*release*/goose_runtime.c"), key=os.path.getmtime)
    return prog, rt


def patched(rt, name, change):
    p = f"{OUT}/rt_{name}.c"
    text = open(rt).read()
    new = change(text)
    open(p, "w").write(new)
    return p


def sub(s, old, new):
    assert s.count(old) >= 1, old
    return s.replace(old, new)


def size(p):
    return os.path.getsize(p) if os.path.exists(p) else -1


def best_bench(exe, bench, n=7):
    vals = []
    for _ in range(n):
        r = run([exe, "1", bench])
        if r.returncode:
            return None
        for line in r.stdout.splitlines():
            if line.startswith("bench"):
                vals.append(float(line.split("\t")[2]))
    return min(vals) if vals else None


def guarded(fn):
    try:
        fn()
    except Exception:
        print("FAILED:", traceback.format_exc()[-1500:], flush=True)


print(run("nproc; lscpu | grep -E 'Model name|Thread|Core'; ldd --version | head -1; clang --version | head -1", shell=True).stdout, flush=True)
print(run(ZIG + ["--version"]).stdout.splitlines()[:1], flush=True)


# ---- ideas for the locks and for waking threads
def locks():
    section("threads and messages: variants of the runtime on one machine")
    prog, rt = gust_c("compare/concurrency/conc.goose", "conc")

    def ttas(s):
        return sub(s, """            if (!pthread_mutex_trylock(m)) return;
""", """            /* Read before the locked try, after the first: a failed try still takes the line. */
            if ((i == 0 || !__atomic_load_n(&m->__data.__lock, __ATOMIC_RELAXED)) && !pthread_mutex_trylock(m)) return;
""")

    def chain(s):
        s = sub(s, "static int64_t gs_threads_sleeping;", "static int gs_threads_waking;\nstatic int64_t gs_threads_sleeping;")
        s = sub(s, """        if (gs_threads_sleeping) gs_cond_signal(&gs_threads_more);
""", """        if (gs_threads_sleeping && !gs_threads_waking) { gs_threads_waking = 1; gs_cond_signal(&gs_threads_more); }
""")
        s = sub(s, """            gs_threads_sleeping--;
            /* A starter counts this thread out as it leaves one here. */
""", """            gs_threads_sleeping--;
            gs_threads_waking = 0;
            /* A starter counts this thread out as it leaves one here. */
""")
        s = sub(s, """                GS_SPIN_STORE(&gs_thread_next, t->handed);
""", """                GS_SPIN_STORE(&gs_thread_next, t->handed);
                if (gs_thread_next && gs_threads_sleeping) { gs_threads_waking = 1; gs_cond_signal(&gs_threads_more); }
""")
        return s

    variants = {"as is": lambda s: s, "read first": ttas, "chain wake": chain, "both": lambda s: chain(ttas(s))}
    exes = {}
    for name, change in variants.items():
        c = patched(rt, name.replace(" ", "_"), change)
        exe = f"{OUT}/conc_{name.replace(' ', '_')}"
        r = run(["clang", *FLAGS, prog, c, "-o", exe, "-lpthread", "-lm"])
        assert r.returncode == 0, r.stderr[-800:]
        exes[name] = exe
    print(f"{'bench':<11}" + "".join(f"{n:>12}" for n in exes), flush=True)
    for b in ("spawnjoin", "spawnmany", "spawnwork", "pingpong", "fanout"):
        best = {}
        for _ in range(9):
            for n, e in exes.items():
                v = float(run([e, "1", b]).stdout.split("\t")[2])
                best[n] = min(best.get(n, 1e9), v)
        print(f"{b:<11}" + "".join(f"{best[n]:12.4f}" for n in exes) + "   " + " ".join(f"{best[n] / best['as is']:.2f}x" for n in exes), flush=True)


# ---- static, and cross, through zig cc
def static():
    section("hello world: size, memory, start to exit")
    hello = f"{OUT}/hello.goose"
    open(hello, "w").write('import os;\n\nfn main() {\n    print("hello");\n    let a = args();\n    if a.len > 1 { sleep(0.6); }\n}\n')
    prog, rt = gust_c(hello, "hello")
    progs = {}
    r = run(["clang", *FLAGS, prog, rt, "-o", f"{OUT}/h_dyn", "-Wl,--gc-sections", "-s", "-lm"])
    assert r.returncode == 0, r.stderr[-500:]
    progs["gust glibc dynamic"] = f"{OUT}/h_dyn"
    r = run(["clang", *FLAGS, prog, rt, "-static", "-o", f"{OUT}/h_static", "-Wl,--gc-sections", "-s", "-lm"])
    print("glibc static:", "ok" if r.returncode == 0 else r.stderr[-300:], flush=True)
    if r.returncode == 0:
        progs["gust glibc static"] = f"{OUT}/h_static"
    r = run(ZIG + ["-target", "x86_64-linux-musl", *FLAGS, "-fno-sanitize=undefined", prog, rt, "-o", f"{OUT}/h_musl", "-Wl,--gc-sections", "-s"])
    print("zig cc musl:", "ok" if r.returncode == 0 else r.stderr[-1500:], flush=True)
    if r.returncode == 0:
        progs["gust musl static (zig cc)"] = f"{OUT}/h_musl"
    open(f"{OUT}/h.rs", "w").write('fn main() { println!("hello"); if std::env::args().count() > 1 { std::thread::sleep(std::time::Duration::from_millis(600)); } }\n')
    if run(["rustc", "-C", "opt-level=3", "-o", f"{OUT}/h_rust", f"{OUT}/h.rs"]).returncode == 0:
        progs["rust"] = f"{OUT}/h_rust"
    os.makedirs(f"{OUT}/hgo", exist_ok=True)
    open(f"{OUT}/hgo/main.go", "w").write('package main\n\nimport ("fmt"; "os"; "time")\n\nfunc main() {\n\tfmt.Println("hello")\n\tif len(os.Args) > 1 { time.Sleep(600 * time.Millisecond) }\n}\n')
    open(f"{OUT}/hgo/go.mod", "w").write("module h\n\ngo 1.21\n")
    if run(["go", "build", "-o", f"{OUT}/h_go", "."], cwd=f"{OUT}/hgo").returncode == 0:
        progs["go"] = f"{OUT}/h_go"
    print(f"{'program':<28}{'bytes':>10}{'RSS KB':>9}{'PSS KB':>9}{'private KB':>12}{'start-exit ms':>15}", flush=True)
    for name, exe in progs.items():
        p = subprocess.Popen([exe, "sleep"], stdout=subprocess.DEVNULL)
        time.sleep(0.3)
        fields = {}
        for line in open(f"/proc/{p.pid}/smaps_rollup"):
            parts = line.split()
            if len(parts) >= 2 and parts[0].rstrip(":") in ("Rss", "Pss", "Private_Clean", "Private_Dirty"):
                fields[parts[0].rstrip(":")] = int(parts[1])
        p.wait()
        best = 1e9
        for _ in range(100):
            t0 = time.perf_counter()
            subprocess.run([exe], stdout=subprocess.DEVNULL)
            best = min(best, time.perf_counter() - t0)
        print(f"{name:<28}{size(exe):>10}{fields.get('Rss', -1):>9}{fields.get('Pss', -1):>9}{fields.get('Private_Clean', 0) + fields.get('Private_Dirty', 0):>12}{best * 1000:>15.3f}", flush=True)

    section("zig cc as the compiler behind `gust build --cc`, with no change to Gust")
    for name, target in (("linux x86-64", "x86_64-linux-musl"), ("linux arm64", "aarch64-linux-musl"), ("windows x86-64", "x86_64-windows-gnu"), ("macos arm64", "aarch64-macos")):
        wrap = f"{OUT}/zigcc_{target}"
        open(wrap, "w").write(f'#!/bin/sh\nexec "{sys.executable}" -m ziglang cc -target {target} "$@"\n')
        os.chmod(wrap, 0o755)
        exe = f"{OUT}/x_{target}"
        r = run([GUST, "build", "-q", "--cc", wrap, hello, "-o", exe])
        kind = run(["file", "-b", exe]).stdout.strip()[:70] if os.path.exists(exe) else ""
        print(f"  gust build --cc zig ({name}): exit {r.returncode}, {size(exe)} bytes, {kind}", flush=True)
        if r.returncode:
            print("   ", (r.stdout + r.stderr)[-500:].replace("\n", "\n    "), flush=True)
        else:
            c = run([exe]) if target == "x86_64-linux-musl" else None
            if c:
                print("    runs here:", c.stdout.strip(), flush=True)
        # And straight, the program's C and the runtime's.
        exe2 = f"{OUT}/y_{target}"
        r = run(ZIG + ["-target", target, "-O2", "-w", "-fno-sanitize=undefined", prog, rt, "-o", exe2])
        print(f"  zig cc on the C directly ({name}): exit {r.returncode}, {size(exe2)} bytes", flush=True)
        if r.returncode:
            print("   ", r.stderr[-400:].replace("\n", "\n    "), flush=True)

    section("the eight benchmarks: glibc dynamic against musl static")
    prog, rt = gust_c("compare/gust/main.goose", "main")
    r = run(["clang", *FLAGS, prog, rt, "-o", f"{OUT}/m_dyn", "-Wl,--gc-sections", "-s", "-lm"])
    assert r.returncode == 0, r.stderr[-500:]
    r = run(ZIG + ["-target", "x86_64-linux-musl", *FLAGS, "-fno-sanitize=undefined", prog, rt, "-o", f"{OUT}/m_musl", "-Wl,--gc-sections", "-s"])
    if r.returncode:
        print("musl failed:", r.stderr[-1000:], flush=True)
        return
    print(f"sizes: dynamic {size(OUT + '/m_dyn')}, musl static {size(OUT + '/m_musl')}", flush=True)
    ratios = []
    for b in ("nbody", "raytrace", "interp", "sort", "astar", "bintrees", "hashmap", "strings"):
        d, m = 1e9, 1e9
        rd = rm = 0
        for _ in range(5):
            r = run(["/usr/bin/time", "-f", "%M", f"{OUT}/m_dyn", "1", b])
            d = min(d, float(r.stdout.split("\t")[2]))
            rd = int(r.stderr.strip().splitlines()[-1])
            r = run(["/usr/bin/time", "-f", "%M", f"{OUT}/m_musl", "1", b])
            m = min(m, float(r.stdout.split("\t")[2]))
            rm = int(r.stderr.strip().splitlines()[-1])
        ratios.append(m / d)
        print(f"  {b:<10} dynamic {d:.4f}  musl {m:.4f}  {m / d:.2f}x   peak KB {rd} -> {rm}", flush=True)
    print(f"  geomean {statistics.geometric_mean(ratios):.3f}x", flush=True)
    prog, rt = gust_c("compare/concurrency/conc.goose", "conc")
    r = run(ZIG + ["-target", "x86_64-linux-musl", *FLAGS, "-fno-sanitize=undefined", prog, rt, "-o", f"{OUT}/c_musl"])
    r2 = run(["clang", *FLAGS, prog, rt, "-o", f"{OUT}/c_dyn", "-lpthread", "-lm"])
    if r.returncode == 0 and r2.returncode == 0:
        for b in ("spawnjoin", "spawnwork", "pingpong", "fanout"):
            print(f"  {b:<10} dynamic {best_bench(OUT + '/c_dyn', b, 5):.4f}  musl {best_bench(OUT + '/c_musl', b, 5):.4f}", flush=True)
    else:
        print("conc under musl failed:", r.stderr[-600:], flush=True)


# ---- stdout
def stdout():
    section("300,000 prints: a write each against a buffer")
    g = f"{OUT}/lines.goose"
    open(g, "w").write('fn main() {\n    for i in 300000 {\n        print("line ", i, " value ", i * 3);\n    }\n}\n')
    prog, rt = gust_c(g, "lines")

    def buffered(s):
        s = sub(s, "setvbuf(stdout, NULL, _IONBF, 0);", "setvbuf(stdout, NULL, _IOFBF, 65536);")
        return sub(s, "GS_API void gs_out_bytes(const uint8_t *p, int64_t len) { gs_write_stream(stdout, p, len); }",
                   "GS_API void gs_out_bytes(const uint8_t *p, int64_t len) { fwrite(p, 1, (size_t)len, stdout); }")

    progs = {}
    run(["clang", *FLAGS, prog, rt, "-o", f"{OUT}/l_as_is", "-lm"])
    progs["gust as is"] = f"{OUT}/l_as_is"
    c = patched(rt, "buffered", buffered)
    r = run(["clang", *FLAGS, prog, c, "-o", f"{OUT}/l_buf", "-lm"])
    assert r.returncode == 0, r.stderr[-500:]
    progs["gust buffered"] = f"{OUT}/l_buf"
    open(f"{OUT}/l.rs", "w").write('fn main() {\n    for i in 0..300000i64 {\n        println!("line {} value {}", i, i * 3);\n    }\n}\n')
    if run(["rustc", "-C", "opt-level=3", "-o", f"{OUT}/l_rust", f"{OUT}/l.rs"]).returncode == 0:
        progs["rust"] = f"{OUT}/l_rust"
    os.makedirs(f"{OUT}/lgo", exist_ok=True)
    open(f"{OUT}/lgo/main.go", "w").write('package main\n\nimport "fmt"\n\nfunc main() {\n\tfor i := 0; i < 300000; i++ {\n\t\tfmt.Println("line", i, "value", i*3)\n\t}\n}\n')
    open(f"{OUT}/lgo/go.mod", "w").write("module l\n\ngo 1.21\n")
    if run(["go", "build", "-o", f"{OUT}/l_go", "."], cwd=f"{OUT}/lgo").returncode == 0:
        progs["go"] = f"{OUT}/l_go"
    for name, exe in progs.items():
        best = 1e9
        for _ in range(5):
            with open(f"{OUT}/lines.out", "wb") as f:
                t0 = time.perf_counter()
                subprocess.run([exe], stdout=f)
                best = min(best, time.perf_counter() - t0)
        best2 = 1e9
        for _ in range(3):
            t0 = time.perf_counter()
            subprocess.run(f"{exe} | cat > /dev/null", shell=True)
            best2 = min(best2, time.perf_counter() - t0)
        print(f"  {name:<16} to a file {best:.3f} s   to a pipe {best2:.3f} s", flush=True)


guarded(locks)
guarded(static)
guarded(stdout)
print("\ndone", flush=True)
