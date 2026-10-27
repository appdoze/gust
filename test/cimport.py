"""Gust: `gust cimport` (src/cimport.h). A header with one of each shape a C
library's interface has becomes a package, and a program calls through it:
numbers and bool, a C string in and out, an out parameter, byte buffers C
fills, a const array, a struct with padding by pointer, by value and as a
result, an opaque handle C owns and frees, a callback C calls through
c_callback, integer, unsigned and string macros and enum constants, and a
variadic function and a float macro left out. The library's implementation
is the package's own C (--source), as a library shipped as source is.
Variadic functions come with a form per kind of extra argument (an option
setter and a printf), a typedef of void is a handle, a `void *` with a
length after it a buffer and one without a handle, and pointer constants
are numbers while an address is no constant."""
import subprocess
from pathlib import Path

import toolchain as tc

HEADER = r'''
#ifndef MINI_H
#define MINI_H
#include <stdint.h>
#include <stddef.h>

#define MINI_VERSION_NUM 10203
#define MINI_NEG (-7)
#define MINI_BIG 0xFFFFFFFFFFFFFFF0ULL
#define MINI_NAME "mini \"lib\""
#define MINI_HALF 0.5
#define MINI_MAX(a, b) ((a) > (b) ? (a) : (b))

enum mini_color { MINI_RED, MINI_GREEN = 5, MINI_BLUE };

typedef struct mini_point { char tag; int64_t x; int32_t y; } mini_point;
typedef struct { uint8_t r, g, b; double weight; } mini_pixel;
typedef struct mini_box mini_box;     /* opaque: only ever a pointer */
typedef int (*mini_cmp)(const void *a, const void *b);
typedef void mini_handle;            /* a handle C hands out, as libcurl's CURL */
typedef void (*mini_free_fn)(void *);
#define MINI_STATIC ((mini_free_fn)0)
#define MINI_TRANSIENT ((mini_free_fn)-1)
#define MINI_SELF ((void *)&mini_add)

int mini_add(int a, int b);
double mini_scale(double x, float f);
_Bool mini_even(long long n);
size_t mini_strlen(const char *s);
const char *mini_greeting(void);
void mini_divmod(int a, int b, int *q, int *r);
int mini_fill(unsigned char *buf, size_t n, unsigned char v);
long mini_sum(const int32_t *xs, size_t n);
void mini_move(mini_point *p, int64_t dx);
int64_t mini_norm1(mini_point p);
mini_pixel mini_grey(mini_pixel p);
mini_box *mini_box_new(int v);
int mini_box_get(const mini_box *b);
void mini_box_free(mini_box *b);
int mini_sort_first(int32_t *xs, size_t n, mini_cmp cmp);
int mini_printf(const char *fmt, ...);
mini_handle *mini_open(int v);
int mini_opt(mini_handle *h, int opt, ...);
void *mini_alloc(int n);
void mini_release(void *p);
int mini_zero(void *buf, size_t n);
#endif
'''

IMPL = r'''
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "mini.h"
int mini_add(int a, int b) { return a + b; }
double mini_scale(double x, float f) { return x * f; }
_Bool mini_even(long long n) { return n % 2 == 0; }
size_t mini_strlen(const char *s) { return strlen(s); }
const char *mini_greeting(void) { return "hello from C"; }
void mini_divmod(int a, int b, int *q, int *r) { *q = a / b; *r = a % b; }
int mini_fill(unsigned char *buf, size_t n, unsigned char v) { memset(buf, v, n); return (int)n; }
long mini_sum(const int32_t *xs, size_t n) { long s = 0; for (size_t i = 0; i < n; i++) s += xs[i]; return s; }
void mini_move(mini_point *p, int64_t dx) { p->x += dx; p->y -= 1; p->tag = 'm'; }
int64_t mini_norm1(mini_point p) { return (p.x < 0 ? -p.x : p.x) + (p.y < 0 ? -p.y : p.y) + p.tag; }
mini_pixel mini_grey(mini_pixel p) {
    uint8_t v = (uint8_t)((p.r + p.g + p.b) / 3);
    mini_pixel q = { v, v, v, p.weight * 2 };
    return q;
}
struct mini_box { int v; };
mini_box *mini_box_new(int v) { mini_box *b = malloc(sizeof *b); b->v = v; return b; }
int mini_box_get(const mini_box *b) { return b->v; }
void mini_box_free(mini_box *b) { free(b); }
int mini_sort_first(int32_t *xs, size_t n, mini_cmp cmp) { qsort(xs, n, sizeof *xs, cmp); return xs[0]; }
int mini_printf(const char *fmt, ...) {
    va_list ap;
    int r = 0;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { r++; continue; }
        switch (*++fmt) {
            case 'd': r += va_arg(ap, int); break;
            case 'f': r += (int)va_arg(ap, double); break;
            case 's': r += (int)strlen(va_arg(ap, const char *)); break;
            default: va_end(ap); return -1;
        }
    }
    va_end(ap);
    return r;
}
struct mini_state { long n; };
mini_handle *mini_open(int v) { struct mini_state *s = calloc(1, sizeof *s); s->n = v; return s; }
int mini_opt(mini_handle *h, int opt, ...) {
    struct mini_state *s = h;
    va_list ap;
    int r = -1;
    va_start(ap, opt);
    switch (opt) {
        case 0: s->n += va_arg(ap, long); r = (int)s->n; break;
        case 1: r = (int)(va_arg(ap, double) * 10); break;
        case 2: r = va_arg(ap, mini_handle *) == h; break;
        case 3: r = (int)strlen(va_arg(ap, const char *)); break;
        case 4: memcpy(va_arg(ap, char *), "ok", 2); r = 2; break;
        case 5: *va_arg(ap, long *) = s->n * 2; r = 1; break;
    }
    va_end(ap);
    return r;
}
void *mini_alloc(int n) { return calloc(1, (size_t)n); }
void mini_release(void *p) { free(p); }
int mini_zero(void *buf, size_t n) { memset(buf, 0, n); return (int)n; }
'''

PROGRAM = r'''
import mini from "./mini";
import ffi;

// qsort's comparator: the two elements' addresses, read back as numbers.
fn at(p: u64) -> i64 {
    let b = ffi::bytes(p, 4);
    var v: i64 = b[1];
    v = v * 256 + b[0];
    return v;
}
export fn by_value(a: u64, b: u64) -> i32 {
    let x = at(a);
    let y = at(b);
    return if x < y { -1 } else if x > y { 1 } else { 0 };
}

fn main() {
    print(mini::MINI_VERSION_NUM, " ", mini::MINI_NEG, " ", mini::MINI_BIG, " ", mini::MINI_NAME);
    print(mini::MINI_RED, " ", mini::MINI_GREEN, " ", mini::MINI_BLUE);
    print(mini::mini_add(40, 2), " ", mini::mini_scale(2.5, 4.0), " ", mini::mini_even(6), " ",
          mini::mini_even(7));
    print(mini::mini_strlen("seven ch"), " ", mini::mini_greeting_str());
    var q: i32[1] = [0];
    var r: i32[1] = [0];
    mini::mini_divmod(17, 5, q, r);
    print(q[0], " ", r[0]);
    var buf: u8[5] = [0, 0, 0, 0, 0];
    print(mini::mini_fill(buf, 3, 7), " ", buf[0], buf[2], buf[3]);
    let xs: i32[4] = [5, -2, 9, 1];
    print(mini::mini_sum(xs, 4));
    var p = mini::mini_point { tag: 97, x: 10, y: 4 };
    mini::mini_move(p, 5);
    print(p.tag, " ", p.x, " ", p.y, " ", mini::mini_norm1(p));
    var px = mini::mini_pixel { r: 10, g: 20, b: 60, weight: 1.25 };
    var grey = mini::mini_pixel { r: 0, g: 0, b: 0, weight: 0.0 };
    mini::mini_grey(grey, px);
    print(grey.r, " ", grey.g, " ", grey.b, " ", grey.weight);
    let box = mini::mini_box_new(33);
    print(mini::mini_box_get(box));
    mini::mini_box_free(box);
    var ys: i32[4] = [300, 7, 42, 9];
    print(mini::mini_sort_first(ys, 4, c_callback(by_value)), " ", ys[3]);
    print(mini::MINI_STATIC, " ", mini::MINI_TRANSIENT);
    let h = mini::mini_open(5);
    print(mini::mini_opt_int(h, 0, 37), " ", mini::mini_opt_f64(h, 1, 2.5), " ", mini::mini_opt_ptr(h, 2, h), " ",
          mini::mini_opt_str(h, 3, "four"));
    var ok: u8[2] = [0, 0];
    var twice: i64[1] = [0];
    print(mini::mini_opt_buf(h, 4, ok), " ", ok[0], ok[1], " ", mini::mini_opt_out(h, 5, twice), " ", twice[0]);
    mini::mini_release(h);
    print(mini::mini_printf("plain"), " ", mini::mini_printf_int("[%d]", 1234), " ",
          mini::mini_printf_str("<%s>", "abc"), " ", mini::mini_printf_f64("%f!", 7.9));
    var z: u8[3] = [9, 9, 9];
    print(mini::mini_zero(z, 2), " ", z[0], z[1], z[2]);
    mini::mini_release(mini::mini_alloc(16));
}
'''

EXPECTED = '''10203 -7 18446744073709551600 mini "lib"
0 5 6
42 10.0 true false
8 hello from C
3 2
3 770
13
109 15 3 127
30 30 30 2.5
33
7 300
0 18446744073709551615
42 25 1 4
2 111107 1 84
5 1236 5 8
2 009
'''


def check_cimport(exe, cc, base, *, extra=()):
    """Raises RuntimeError on a failure. `cc` None skips it: the program's C
    has to be built by something."""
    if not cc:
        return
    base = (Path(base) / "cimport").resolve()
    base.mkdir(parents=True, exist_ok=True)
    tc.write_text(base / "mini.h", HEADER)
    tc.write_text(base / "mini_impl.c", IMPL)
    r = subprocess.run([str(exe), "cimport", str(base / "mini.h"), "--source", str(base / "mini_impl.c"),
                        "--out", str(base)], capture_output=True,
                       text=True, errors="replace", timeout=300)
    if r.returncode:
        raise RuntimeError(f"gust cimport:\n{r.stdout}{r.stderr}")
    gen = (base / "mini" / "mini.goose").read_text(encoding="utf-8")
    for want in ("// mini_printf takes a variable number of arguments", "fn mini_opt_out(h: u64, opt: i32, v: i64[:])",
                 "fn mini_release(p: u64)", "fn mini_zero(buf: u8[:], n: u64)"):
        if want not in gen:
            raise RuntimeError(f"cimport should say `{want}`:\n{gen}")
    if "MINI_HALF" in gen or "MINI_MAX" in gen or "MINI_SELF" in gen:
        raise RuntimeError(f"cimport imported a float, an address or a function-like macro:\n{gen}")
    # The library's implementation is the package's own C (--source), as a
    # library shipped as source is.
    tc.write_text(base / "prog.goose", PROGRAM)
    (base / "prog.build").unlink(missing_ok=True)

    def run(argv):
        p = subprocess.run([str(a) for a in argv], cwd=base, capture_output=True, text=True, errors="replace",
                           timeout=600)
        if tc.sanitizer_failure(p.stderr):
            raise RuntimeError(p.stdout + p.stderr)
        return p
    p = run([exe, "run", "-q", base / "prog.goose"])
    if p.returncode or p.stdout.replace("\r\n", "\n") != EXPECTED:
        raise RuntimeError(f"gust run prog.goose, exit {p.returncode}:\n{p.stdout}{p.stderr}\nwanted:\n{EXPECTED}")
    out = base / ("prog" + tc.EXE_SUFFIX)
    p = run([exe, "build", "-q", base / "prog.goose", "-o", out])
    if p.returncode:
        raise RuntimeError(f"gust build prog.goose:\n{p.stdout}{p.stderr}")
    p = run([out])
    if p.returncode or p.stdout.replace("\r\n", "\n") != EXPECTED:
        raise RuntimeError(f"prog, exit {p.returncode}:\n{p.stdout}{p.stderr}\nwanted:\n{EXPECTED}")
