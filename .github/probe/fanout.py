"""Temporary: why typed queues are slow on Linux with several threads at one queue.
Each variant patches the runtime's C text and compiles it with the program's C, so
that it really is another program: the compiler itself only uses its embedded runtime."""
import glob, os, re, shutil, statistics, subprocess, sys

GUST = os.path.abspath("build/goose" if os.path.exists("build/goose") else "build/gust")
HOME = os.path.expanduser("~")


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def gust_build(src, exe):
    r = run([GUST, "build", "-q", "--cc", "clang", src, "-o", exe])
    assert r.returncode == 0, r.stdout + r.stderr


os.makedirs("/tmp/p", exist_ok=True)
conc = open("compare/concurrency/conc.goose").read()
for n in (1, 2, 3):
    text = conc.replace("for w in 3 { ids.push(thread_spawn(consumer, w)); }", f"for w in {n} {{ ids.push(thread_spawn(consumer, w)); }}") \
               .replace("for w in 3 { qput(Job { n: -1 }); }", f"for w in {n} {{ qput(Job {{ n: -1 }}); }}") \
               .replace("for w in 3 { sum += qget<Done>().sum; }", f"for w in {n} {{ sum += qget<Done>().sum; }}")
    assert text != conc or n == 3
    open(f"/tmp/p/conc{n}.goose", "w").write(text)
    gust_build(f"/tmp/p/conc{n}.goose", f"/tmp/p/conc{n}")

prog = {n: max(glob.glob(f"{HOME}/.gust/build/prog/conc{n}-*/conc{n}.c"), key=os.path.getmtime) for n in (1, 2, 3)}
rt = max(glob.glob(f"{HOME}/.gust/build/runtime/*/clang*release*/goose_runtime.c"), key=os.path.getmtime)
RT = open(rt).read()
print("runtime", rt, len(RT), "bytes; GS_QSPIN 2000 there:", RT.count("#define GS_QSPIN 2000"), flush=True)
FLAGS = "-O2 -ffunction-sections -fdata-sections -fno-stack-protector -fcf-protection=none -fno-asynchronous-unwind-tables -w".split()


def sub(s, old, new):
    assert s.count(old) >= 1, old
    return s.replace(old, new)


LOOK = """            gs_mutex_unlock(&s->mutex);
        }
        gs_pf_relax();
    }
    gs_qlock(s);
    while (!s->count) {"""

VARIANTS = {
    "as is": lambda s: s,
    "spin 64": lambda s: sub(s, "#define GS_QSPIN 2000", "#define GS_QSPIN 64"),
    "spin 16": lambda s: sub(s, "#define GS_QSPIN 2000", "#define GS_QSPIN 16"),
    "no lock spin": lambda s: sub(s, "#define GS_QLOCK_SPIN 128", "#define GS_QLOCK_SPIN 0"),
    "no pause": lambda s: sub(s, "__builtin_ia32_pause();", "__asm__ __volatile__(\"\");"),
    "look /16": lambda s: sub(s, LOOK, LOOK.replace("        gs_pf_relax();\n", "        for (int r = 0; r < 16; r++) gs_pf_relax();\n")),
    "look /128": lambda s: sub(s, LOOK, LOOK.replace("        gs_pf_relax();\n", "        for (int r = 0; r < 128; r++) gs_pf_relax();\n")),
    "lost race /256": lambda s: sub(s, """            gs_mutex_unlock(&s->mutex);
        }
        gs_pf_relax();
    }
    gs_qlock(s);
    while (!s->count) {""", """            gs_mutex_unlock(&s->mutex);
            for (int r = 0; r < 256; r++) gs_pf_relax();
        }
        gs_pf_relax();
    }
    gs_qlock(s);
    while (!s->count) {"""),
    "after take /64": lambda s: sub(s, """                gs_qnode *n = gs_qtake(s);
                gs_mutex_unlock(&s->mutex);
                return n;
            }
            gs_mutex_unlock(&s->mutex);
        }
        gs_pf_relax();""", """                gs_qnode *n = gs_qtake(s);
                gs_mutex_unlock(&s->mutex);
                for (int r = 0; r < 64; r++) gs_pf_relax();
                return n;
            }
            gs_mutex_unlock(&s->mutex);
        }
        gs_pf_relax();"""),
    "lines apart": lambda s: sub(s, """    int64_t count, limit;
    gs_mutex mutex;
    gs_cond cond;
    struct gs_qbox_entry *boxes;""", """    char pad0[64];
    int64_t count, limit;
    char pad1[64];
    gs_mutex mutex;
    gs_cond cond;
    char pad2[64];
    struct gs_qbox_entry *boxes;"""),
    "adaptive": lambda s: sub(s, """    gs_mutex_init(&s->mutex);
    gs_cond_init_timed(&s->cond);""", """    {
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, 3);
        pthread_mutex_init(&s->mutex, &a);
        pthread_mutexattr_destroy(&a);
    }
    gs_cond_init_timed(&s->cond);"""),
}


def build_variant(name, change, n=3):
    d = f"/tmp/p/v_{re.sub(r'[^a-z0-9]', '', name)}_{n}"
    os.makedirs(d, exist_ok=True)
    c = f"{d}/rt.c"
    open(c, "w").write(change(RT))
    exe = f"{d}/conc"
    r = run(["clang", *FLAGS, prog[n], c, "-o", exe, "-lpthread", "-lm"])
    assert r.returncode == 0, r.stderr[-800:]
    return exe


def fan(exe, prefix=()):
    times = []
    timed = None
    for i in range(5):
        r = run([*prefix, "/usr/bin/time", "-f", "%U %S %w %c", exe, "1", "fanout"])
        times.append(float(r.stdout.split("\t")[2]))
        timed = r.stderr.strip().splitlines()[-1].split()
    return times, timed


print(run("nproc; lscpu | grep -E 'Model name|Thread|Core|L3|Hypervisor'; lscpu -e; uname -r", shell=True).stdout, flush=True)
print(f"{'variant':<16}{'best':>8}{'median':>8}{'user':>8}{'sys':>8}{'vol':>9}{'invol':>8}{'pingpong':>10}{'spawnwork':>10}", flush=True)
for name, change in VARIANTS.items():
    exe = build_variant(name, change)
    times, t = fan(exe)
    other = {b: min(float(run([exe, "1", b]).stdout.split("\t")[2]) for _ in range(3)) for b in ("pingpong", "spawnwork")}
    print(f"{name:<16}{min(times):8.3f}{statistics.median(times):8.3f}{t[0]:>8}{t[1]:>8}{t[2]:>9}{t[3]:>8}{other['pingpong']:10.4f}{other['spawnwork']:10.4f}", flush=True)

print("\nhow many receivers, as is", flush=True)
for n in (1, 2, 3):
    times, t = fan(build_variant("as is", lambda s: s, n))
    print(f"  sender and {n}: best {min(times):.3f} median {statistics.median(times):.3f}", flush=True)

print("\nwhich CPUs, as is, sender and 3 receivers", flush=True)
exe = build_variant("as is", lambda s: s)
for cpus in ("0", "0,1", "0,2", "0,1,2", "0-3"):
    times, t = fan(exe, ("taskset", "-c", cpus))
    print(f"  cpus {cpus:<8} best {min(times):.3f} median {statistics.median(times):.3f} vol {t[2]} invol {t[3]}", flush=True)

print("\nhow large the ring grew, as is", flush=True)
exe = build_variant("grow", lambda s: sub(s, "    int64_t cap = s->cap ? s->cap * 2 : 64;", "    int64_t cap = s->cap ? s->cap * 2 : 64;\n    fprintf(stderr, \"ring %lld\n\", (long long)cap);"))
r = run([exe, "1", "fanout"])
print("  " + " ".join(r.stderr.split()[-12:]), flush=True)
