"""Gust: Python bindings (scripts/gust_python.py). A library's export fns
become a package that loads it with ctypes: integers checked against their
type's range, floats, bools, structs at their natural alignment (built
from a tuple or a dict, nested, returned), slices of numbers or structs
read from a sequence or a buffer in place and written back to a list or a
writable buffer, a returned const u8[:] as bytes, a renamed export, a
parameter named by a Python keyword, a packed struct left out with the
reason, the runtime's own state (a global set by main) and the import's
thread. Where pip and setuptools are installed, the package also goes through pip,
offline, and is imported from where pip put it."""
import difflib
import importlib.util
import os
import subprocess
import sys
from pathlib import Path

import toolchain as tc

LIBRARY = r'''
// A library for Python: one export of each shape the bindings take.
var calls = 0;
fn main() { calls = 100; }

export fn add(a: i64, b: i64) -> i64 { calls += 1; a + b }
export fn tiny(a: i8, b: u16, c: u32) -> i64 { var t: i64 = a; t += b; t += c; return t; }
export fn halve(x: f32, y: f64) -> f64 { x / 2.0 + y / 2.0 }
export fn is_even(n: i64) -> bool { n % 2 == 0 }
export fn calls_so_far() -> i64 { calls }
export "py_mul" fn mul(a: i32, b: i32) -> i32 { a * b }
export fn count(s: const u8[:], c: u8) -> i64 {
    var n = 0;
    for b in s { if b == c { n += 1; } }
    return n;
}
export fn upcase(s: u8[:]) -> i64 {
    var n = 0;
    for b, i in s {
        if b >= 97 && b <= 122 { s[i] = b - 32; n += 1; }
    }
    return n;
}
export fn scale(xs: f64[:], k: f64) { for x, i in xs { xs[i] = x * k; } }
export fn sum(xs: const i64[:]) -> i64 { var t = 0; for x in xs { t += x; } return t; }
export fn greet() -> const u8[:] { return "hello from Gust"; }

struct P { x: i64, y: i64 }
struct Seg { from: P, to: P, w: f64 }
struct Pix { r: u8, g: u8, b: u8, on: bool }
struct Odd { a: u8, n: i64 }
export fn mid(s: Seg) -> P { P { x: (s.from.x + s.to.x) / 2, y: (s.from.y + s.to.y) / 2 } }
export fn grey(p: Pix) -> Pix {
    var t: i64 = p.r;
    t += p.g;
    t += p.b;
    let v = (t / 3) as u8;
    Pix { r: v, g: v, b: v, on: !p.on }
}
export fn total(ps: const P[:]) -> i64 { var t = 0; for p in ps { t += p.x * p.y; } return t; }
export fn shift(ps: P[:], d: i64) { for p, i in ps { ps[i] = P { x: p.x + d, y: p.y }; } }
export fn dist(from: P, to: P) -> i64 { (to.x - from.x) * (to.x - from.x) + (to.y - from.y) * (to.y - from.y) }
export fn odd(o: Odd) -> i64 { o.n + o.a }
'''

DRIVER = r'''
import array, sys, threading
import mylib as m

def err(f, *a):
    try:
        f(*a)
    except (TypeError, OverflowError) as e:
        return f"{type(e).__name__}: {e}"
    return "no error"

print(m.calls_so_far(), m.add(2, 40), m.calls_so_far())
print(m.tiny(-5, 65535, 4000000000), m.halve(3.0, 5.0), m.is_even(10), m.is_even(7), m.py_mul(6, -7))
print(err(m.add, 2 ** 63, 0))
print(err(m.tiny, -129, 0, 0))
print(err(m.tiny, 0, 0, -1))
print(err(m.add, 1.5, 1))
print(err(m.halve, "1", 2))
print(m.count(b"banana", ord("a")), m.count("abé", 0xc3), m.count(bytearray(b"aa"), 97),
      m.count(memoryview(b"xyzzy")[2:], ord("z")), m.count(b"", 0))
b = bytearray(b"Hello, gust!")
print(m.upcase(b), b)
l = [104, 105]
print(m.upcase(l), l)
print(err(m.upcase, b"abc"))
print(err(m.upcase, (104, 105)))
xs = [1.0, 2.5, -3.0]
m.scale(xs, 2.0)
a = array.array("d", [1, 2, 3])
m.scale(a, 10)
print(xs, a)
print(err(m.scale, array.array("q", [1]), 2.0))
print(m.sum([1, 2, 3, 4]), m.sum(()), m.sum(array.array("q", [5, 6])), m.sum(range(10)))
print(err(m.sum, [1, 2.5]))
print(m.greet())
s = m.mid(((0, 0), (10, 20), 1.5))
print(s, tuple(s), s == m.P(5, 10), m.mid({"from_": (2, 2), "to": m.P(4, 6), "w": 0}))
print(m.grey(m.Pix(10, 20, 30, True)))
print(err(m.grey, (300, 0, 0, False)))
print(err(m.mid, (1, 2)))
print(err(m.mid, {"from": (0, 0), "to": (1, 1), "w": 0}))
ps = [(1, 2), m.P(3, 4)]
print(m.total(ps), m.total([]))
ps = [m.P(1, 2), (3, 4)]
m.shift(ps, 10)
print(ps, m.dist(m.P(1, 1), (4, -3)))
print(hasattr(m, "odd"), hasattr(m, "Odd"), sorted(m.__all__))
src = open(m.__file__, encoding="utf-8").read()
print([line for line in src.splitlines() if "not bound" in line])
out = []
def elsewhere():
    try:
        m.add(1, 2)
        out.append("no error")
    except RuntimeError as e:
        out.append(f"RuntimeError: {e}")
t = threading.Thread(target=elsewhere)
t.start()
t.join()
print(out[0])
'''

EXPECTED = '''100 42 101
4000065530 4.0 True False -42
OverflowError: add(a): 9223372036854775808 is outside [-9223372036854775808, 9223372036854775807]
OverflowError: tiny(a): -129 is outside [-128, 127]
OverflowError: tiny(c): -1 is outside [0, 4294967295]
TypeError: add(a): an integer, not float
TypeError: halve(x): a number, not str
3 1 2 2 0
8 bytearray(b'HELLO, GUST!')
2 [72, 73]
TypeError: upcase(s): the function may write to it, and this buffer is read only
TypeError: upcase(s): the function may write to it, so it takes a list or a writable buffer
[2.0, 5.0, -6.0] array('d', [10.0, 20.0, 30.0])
TypeError: scale(xs): a buffer of q elements, where c_double ones are wanted
10 0 11 45
TypeError: sum(xs)[1]: an integer, not float
b'hello from Gust'
P(x=5, y=10) (5, 10) True P(x=3, y=4)
Pix(r=20, g=20, b=20, on=False)
OverflowError: grey(p).r: 300 is outside [0, 255]
TypeError: mid(s): a Seg has 3 fields, not 2
TypeError: mid(s): a Seg has the fields from_, to, w
14 0
[P(x=11, y=2), P(x=13, y=4)] 25
False False ['P', 'Pix', 'Seg', 'add', 'calls_so_far', 'count', 'dist', 'greet', 'grey', 'halve', 'is_even', 'mid', 'py_mul', 'scale', 'shift', 'sum', 'tiny', 'total', 'upcase']
['# struct Odd: not bound, since its field n is at offset 1, not at a multiple of 8.', '# odd(o: Odd) -> i64: not bound, since parameter o has type Odd: its field n is at offset 1, not at a multiple of 8.']
RuntimeError: add: mylib's Gust functions run on the thread that imported it
'''


def check_python_bindings(exe, cc, base):
    """Raises RuntimeError on a failure. `cc` is the toolchain's compiler."""
    base = (Path(base) / "python-bindings").resolve()
    base.mkdir(parents=True, exist_ok=True)
    src = base / "mylib.gust"
    tc.write_text(src, LIBRARY)
    out = base / "dist"
    script = tc.REPO_ROOT / "scripts" / "gust_python.py"
    r = subprocess.run([sys.executable, str(script), str(src), "--out", str(out), "--exe", str(exe),
                        "--cc", cc.cc], capture_output=True, text=True, errors="replace", timeout=600)
    if r.returncode:
        raise RuntimeError(f"gust_python.py:\n{r.stdout}{r.stderr}")
    tc.write_text(base / "driver.py", DRIVER)

    def run(where):
        env = dict(os.environ, PYTHONPATH=str(where), PYTHONIOENCODING="utf-8")
        p = subprocess.run([sys.executable, "-W", "error", str(base / "driver.py")], cwd=base, env=env,
                           capture_output=True, text=True, errors="replace", timeout=300)
        if p.returncode or p.stdout.replace("\r\n", "\n") != EXPECTED:
            diff = difflib.unified_diff(EXPECTED.splitlines(), p.stdout.splitlines(), "expected", "printed",
                                        lineterm="")
            raise RuntimeError(f"the bindings from {where}, exit {p.returncode}:\n" + "\n".join(diff)
                               + "\n" + p.stderr)
    run(out)
    if importlib.util.find_spec("setuptools") and importlib.util.find_spec("pip"):
        target = base / "site"
        p = subprocess.run([sys.executable, "-m", "pip", "install", "--no-build-isolation", "--no-index",
                            "--no-deps", "--upgrade", "--target", str(target), str(out)],
                           capture_output=True, text=True, errors="replace", timeout=600)
        if p.returncode:
            raise RuntimeError(f"pip install:\n{p.stdout}{p.stderr}")
        run(target)
