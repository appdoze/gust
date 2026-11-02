#!/usr/bin/env python3
"""Gust: what a buffered stdout must still do (src/runtime/runtime_threads.h): nothing a
program printed goes missing or out of order, whatever way it ends short of being killed
outright, and output that comes slowly comes when it is printed. The rest of the suite
compares stdout and stderr apart, once a program is over; this looks at them on one pipe,
and while it runs.

    python scripts/stdout_tests.py --gust build/gust
"""
import argparse
import glob
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOME = os.path.expanduser("~")
WINDOWS = os.name == "nt"
SLOW = 15.0     # how long a machine as busy as it likes may take over something quick


class Checks:
    def __init__(self, gust):
        self.gust = gust
        self.out = tempfile.mkdtemp(prefix="stdout")
        self.bad = []
        self.count = 0
        self.programs = 0

    def report(self, name, good, detail=""):
        self.count += 1
        print(f"{'ok  ' if good else 'FAIL'} {name}{'' if good else ': ' + str(detail)[:600]}", flush=True)
        if not good:
            self.bad.append(name)

    def build(self, text):
        self.programs += 1
        stem = f"p{self.programs}"
        src = os.path.join(self.out, stem + ".goose")
        with open(src, "w", newline="\n") as f:
            f.write(text)
        exe = os.path.join(self.out, stem + (".exe" if WINDOWS else ""))
        r = subprocess.run([self.gust, "build", "-q", src, "-o", exe], capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"the build failed:\n{text}\n{r.stdout}{r.stderr}")
        return exe, src

    def both(self, exe, env=None):
        """stdout and stderr on one pipe, as `2>&1` has them."""
        p = subprocess.run([exe], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                           timeout=120, env=env)
        return p.returncode, p.stdout.decode(errors="replace").replace("\r\n", "\n")


def read_until(pipe, want, seconds):
    """Whether `want` comes down the pipe within `seconds`, reading it as it comes."""
    got = []
    done = threading.Event()

    def reader():
        while True:
            b = os.read(pipe.fileno(), 65536)
            if not b:
                break
            got.append(b)
            if want in b"".join(got):
                done.set()
                return
        done.set()

    threading.Thread(target=reader, daemon=True).start()
    done.wait(seconds)
    return want in b"".join(got), b"".join(got)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gust", required=True)
    args = ap.parse_args()
    os.chdir(ROOT)
    c = Checks(os.path.abspath(args.gust))
    base = dict(os.environ)
    base.pop("GUST_UNBUFFERED", None)

    # In order on one pipe, with stderr.
    exe, _ = c.build('import os;\nfn main() {\n    for i in 3 {\n        print("out ", i);\n        write_stderr("err\\n");\n    }\n    print("last");\n}\n')
    code, text = c.both(exe, base)
    c.report("stdout and stderr stay in order on one pipe", code == 0 and text == "out 0\nerr\nout 1\nerr\nout 2\nerr\nlast\n", text)

    # A burst, then each way of ending: all of it there, and before the message.
    burst = 'for i in 5000 { print("line ", i); }'
    want = "".join(f"line {i}\n" for i in range(5000))
    endings = {
        "returning from main": ("", 0, ""),
        "exit()": ("import os;\n", 3, "exit(3);"),
        "abort()": ("", None, 'abort("the end");'),
        "a bounds check": ("", None, "var a: i64[>..] = [];\n    let n = a.len + 5;\n    print(a[n]);"),
    }
    for name, (head, wantcode, tail) in endings.items():
        exe, _ = c.build(f"{head}fn main() {{\n    {burst}\n    {tail}\n}}\n")
        code, text = c.both(exe, base)
        good = text.startswith(want) and (wantcode is None and code != 0 or code == wantcode)
        if wantcode is None:
            good = good and "goose runtime error" in text[len(want):]
        c.report(f"a burst then {name}: all of it, first", good, f"exit {code}, {len(text)} bytes, ends {text[-120:]!r}")

    # A fault on the way out: the native stack overflowing, in a recursion no compiler
    # can make a loop of (test/threads/native_stack_overflow.goose).
    exe, _ = c.build(f"recursive fn down(n: i64) -> i64 {{\n    if n < 0 {{ return 0; }}\n    let a: i64[4] = [n, n * 3 + 1, n ^ 5, n * 7];\n    let r = down(a[n % 4] % 1000 + 1);\n    r + a[r % 4]\n}}\nfn main() {{\n    {burst}\n    print(down(1));\n}}\n")
    code, text = c.both(exe, base)
    c.report("a burst then the stack overflowing: all of it, first", text.startswith(want) and code != 0, f"exit {code}, {len(text)} bytes, ends {text[-120:]!r}")

    # Workers printing, in whole lines, and all of them.
    exe, _ = c.build('thread_fn talker(from: i64) {\n    for i in 2000 { print("worker ", from, " line ", i); }\n}\nfn main() {\n    var ids: i64[>..] = [];\n    for w in 4 { ids.push(thread_spawn(talker, w)); }\n    for id in ids { thread_wait(id); }\n}\n')
    code, text = c.both(exe, base)
    lines = text.split("\n")[:-1]
    expect = sorted(f"worker {w} line {i}" for w in range(4) for i in range(2000))
    c.report("workers' lines all there, none cut", code == 0 and sorted(lines) == expect, f"exit {code}, {len(lines)} lines")
    order = all(int(a.split()[3]) < int(b.split()[3]) for w in range(4) for a, b in zip(
        [l for l in lines if l.startswith(f"worker {w} ")], [l for l in lines if l.startswith(f"worker {w} ")][1:]))
    c.report("each worker's lines in its order", order)

    # A worker still printing as main ends must not hang the end, or crash it.
    exe, _ = c.build('thread_fn talker(x: i64) {\n    loop { print("still going"); }\n}\nfn main() {\n    thread_spawn(talker, 0);\n    for i in 100 { print("main ", i); }\n}\n')
    code, text = c.both(exe, base)
    c.report("a worker printing as main ends", code == 0 and "main 99\n" in text, f"exit {code}, {len(text)} bytes")

    # Slow output comes when it is printed.
    exe, _ = c.build('import os;\nfn main() {\n    print("before the sleep");\n    print("and another");\n    sleep_ms(60000);\n}\n')
    p = subprocess.Popen([exe], stdout=subprocess.PIPE, env=base)
    good, got = read_until(p.stdout, b"and another\n", SLOW)
    p.kill()
    p.wait()
    c.report("what waits goes out before a sleep", good, got)

    exe, _ = c.build('import os;\nfn busy(ms: i64) {\n    let t0 = clock();\n    while clock() - t0 < ms as f64 / 1000.0 { }\n}\nfn main() {\n    for i in 3 {\n        print("slow line ", i);\n        busy(200);\n    }\n    busy(60000);\n}\n')
    p = subprocess.Popen([exe], stdout=subprocess.PIPE, env=base)
    good, got = read_until(p.stdout, b"slow line 2\n", SLOW)
    p.kill()
    p.wait()
    c.report("prints far apart come as they are printed, with no block", good, got)

    exe, _ = c.build('import os;\nfn main() {\n    print("the prompt");\n    print("and more");\n    var line: u8[>..] = [];\n    read_line(line);\n    print("read ", line);\n}\n')
    p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=base)
    good, got = read_until(p.stdout, b"and more\n", SLOW)
    c.report("what waits goes out before reading stdin", good, got)
    p.kill()
    p.wait()

    exe, _ = c.build('import os;\nfn main() {\n    print("a");\n    print("b");\n    flush_stdout();\n    let t0 = clock();\n    while clock() - t0 < 60.0 { }\n}\n')
    p = subprocess.Popen([exe], stdout=subprocess.PIPE, env=base)
    good, got = read_until(p.stdout, b"b\n", SLOW)
    p.kill()
    p.wait()
    c.report("flush_stdout flushes", good, got)

    # GUST_UNBUFFERED: the burst's end comes with no block or flush at all.
    exe, _ = c.build('import os;\nfn main() {\n    print("a");\n    print("b");\n    print("c");\n    let t0 = clock();\n    while clock() - t0 < 60.0 { }\n}\n')
    p = subprocess.Popen([exe], stdout=subprocess.PIPE, env={**base, "GUST_UNBUFFERED": "1"})
    good, got = read_until(p.stdout, b"c\n", SLOW)
    p.kill()
    p.wait()
    c.report("GUST_UNBUFFERED writes each print", good, got)

    # A signal mid-burst: what was printed comes out, in whole lines, and the program
    # still ends as killed by it.
    if not WINDOWS:
        exe, _ = c.build('fn main() {\n    var i = 0;\n    loop {\n        print("line ", i);\n        i++;\n    }\n}\n')
        for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            p = subprocess.Popen([exe], stdout=subprocess.PIPE, env=base)
            got = bytearray(os.read(p.stdout.fileno(), 65536))
            p.send_signal(sig)
            while True:
                b = os.read(p.stdout.fileno(), 1 << 20)
                if not b:
                    break
                got += b
            code = p.wait()
            lines = bytes(got).split(b"\n")
            whole = lines[-1] == b"" and all(l == b"line %d" % i for i, l in enumerate(lines[:-1]))
            c.report(f"{sig.name} in a burst: every line whole and in order, ended by it", whole and code == -sig, f"exit {code}, {len(lines)} lines, last {lines[-3:]!r}")

        # What is set to be ignored stays so.
        exe, _ = c.build('import os;\nfn main() {\n    print("up");\n    sleep_ms(1500);\n    print("still up");\n}\n')
        p = subprocess.Popen([exe], stdout=subprocess.PIPE, env=base,
                             preexec_fn=lambda: signal.signal(signal.SIGHUP, signal.SIG_IGN))
        good, _ = read_until(p.stdout, b"up\n", SLOW)
        p.send_signal(signal.SIGHUP)
        code = p.wait(timeout=60)
        c.report("a signal the parent ignored stays ignored", good and code == 0, f"exit {code}")

    # Foreign C prints for itself, so a program with any keeps a write per print.
    exe, src = c.build('fn main() {\n    print("plain");\n}\n')
    stem = os.path.basename(src)[:-len(".goose")]
    gen = max(glob.glob(f"{HOME}/.gust/build/prog/{stem}-*/{stem}.c"), key=os.path.getmtime)
    c.report("a plain program may buffer", "gs_out_begin();" in open(gen, errors="replace").read())
    exe, src = c.build('extern fn c_abs(x: i32) -> i32 = "abs";\nfn main() {\n    print(c_abs(-3));\n}\n')
    stem = os.path.basename(src)[:-len(".goose")]
    gen = max(glob.glob(f"{HOME}/.gust/build/prog/{stem}-*/{stem}.c"), key=os.path.getmtime)
    c.report("a program with foreign C does not", "gs_out_begin();" not in open(gen, errors="replace").read())

    print(f"{c.count} checks, {len(c.bad)} failed: {c.bad}", flush=True)
    sys.exit(1 if c.bad else 0)


if __name__ == "__main__":
    main()
