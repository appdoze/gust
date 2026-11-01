"""Temporary: why typed queues are slow on Linux with several threads at one queue."""
import os, re, shutil, statistics, subprocess, sys

GUST = os.path.abspath("build/goose" if os.path.exists("build/goose") else "build/gust")
RT = "src/runtime/runtime_threads.h"

BASE = open(RT).read()


def sub(s, old, new):
    assert s.count(old) >= 1, old
    return s.replace(old, new)


VARIANTS = {
    "as is": lambda s: s,
    "spin 64": lambda s: sub(s, "#define GS_QSPIN 2000", "#define GS_QSPIN 64"),
    "no lock spin": lambda s: sub(s, "#define GS_QLOCK_SPIN 128", "#define GS_QLOCK_SPIN 0"),
    "neither": lambda s: sub(sub(s, "#define GS_QSPIN 2000", "#define GS_QSPIN 64"), "#define GS_QLOCK_SPIN 128", "#define GS_QLOCK_SPIN 0"),
    "no pause": lambda s: sub(s, "__builtin_ia32_pause();", "__asm__ __volatile__(\"\");"),
    "look less": lambda s: sub(s, """            gs_mutex_unlock(&s->mutex);
        }
        gs_pf_relax();
    }
    gs_qlock(s);
    while (!s->count) {""", """            gs_mutex_unlock(&s->mutex);
        }
        for (int r = 0; r < 16; r++) gs_pf_relax();
    }
    gs_qlock(s);
    while (!s->count) {"""),
    "lines apart": lambda s: sub(sub(s, """    int64_t count, limit;
    gs_mutex mutex;
    gs_cond cond;
    struct gs_qbox_entry *boxes;""", """    char pad0[64];
    int64_t count, limit;
    char pad1[64];
    gs_mutex mutex;
    gs_cond cond;
    char pad2[64];
    struct gs_qbox_entry *boxes;"""), "    int64_t cap, first, esize;\n", "    int64_t cap, first, esize;\n"),
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

print(subprocess.run("nproc; lscpu | grep -E 'Model name|Thread|Core|Hypervisor'; uname -r", shell=True, capture_output=True, text=True).stdout)
print(f"{'variant':<14}{'best':>8}{'median':>8}{'user':>8}{'sys':>8}{'vol':>10}{'invol':>9}{'futex':>10}{'pingpong':>10}{'spawnmany':>10}")
for name, change in VARIANTS.items():
    d = f"/tmp/rt_{re.sub(r'[^a-z]', '', name)}"
    shutil.rmtree(d, ignore_errors=True)
    shutil.copytree("src/runtime", d)
    open(f"{d}/runtime_threads.h", "w").write(change(BASE))
    exe = f"{d}/conc"
    r = subprocess.run([GUST, "build", "-q", "--cc", "clang", "compare/concurrency/conc.goose", "-o", exe], capture_output=True, text=True, env={**os.environ, "GOOSE_RUNTIME": d})
    if r.returncode:
        print(name, "BUILD FAILED", r.stdout[-600:], r.stderr[-600:])
        continue
    times = []
    timed = None
    for i in range(5):
        r = subprocess.run(["/usr/bin/time", "-f", "%U %S %w %c", exe, "1", "fanout"], capture_output=True, text=True)
        times.append(float(r.stdout.split("\t")[2]))
        timed = r.stderr.strip().splitlines()[-1].split()
    s = subprocess.run(f"strace -f -c -e trace=futex {exe} 1 fanout 2>&1 >/dev/null | awk '/futex/ {{print $4}}'", shell=True, capture_output=True, text=True).stdout.strip()
    other = {}
    for b in ("pingpong", "spawnmany"):
        other[b] = min(float(subprocess.run([exe, "1", b], capture_output=True, text=True).stdout.split("\t")[2]) for _ in range(3))
    print(f"{name:<14}{min(times):8.3f}{statistics.median(times):8.3f}{timed[0]:>8}{timed[1]:>8}{timed[2]:>10}{timed[3]:>9}{s:>10}{other['pingpong']:10.4f}{other['spawnmany']:10.4f}", flush=True)
