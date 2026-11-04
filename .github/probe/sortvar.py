"""Variants of std's sort, each as a copy of stdlib/ patched, for GOOSE_STDLIB.

    python sortvar.py <stdlib dir> <out dir>
Leaves <out dir>/<variant>/ for each variant.
"""
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sortnets import NET9, NET13


def net_fn(name, net):
    steps = "\n".join(f"    sort_step(v, {a}, {b}) {{ a, b => F(a, b) }};" for a, b in net)
    return f"fn {name}<T, F>(v: T[:]) {{\n{steps}\n}}\n"


SMALL = """// One step of a sorting network: v[a] and v[b] in order, with no branch on what they hold.
inline fn sort_step<T, F>(v: T[:], a: i64, b: i64) {
    let x: T = v[a];
    let y: T = v[b];
    let swap = F(y, x);
    v[a] .= if swap { y } else { x };
    v[b] .= if swap { x } else { y };
}

""" + net_fn("sort_net9", NET9) + "\n" + net_fn("sort_net13", NET13) + """
// v in order, by a network as far as one goes and insertion from there.
fn sort_presort<T, F>(v: T[:]) {
    var from = 1;
    if v.len >= 13 {
        sort_net13(v) { a, b => F(a, b) };
        from = 13;
    } else if v.len >= 9 {
        sort_net9(v) { a, b => F(a, b) };
        from = 9;
    }
    for i in from..v.len {
        let x: T = v[i];
        var j = i;
        while j > 0 {
            let y: T = v[j - 1];
            if !F(x, y) { break; }
            v[j] .= y;
            j -= 1;
        }
        v[j] .= x;
    }
}

"""

SORT_OLD_HEAD = """fn sort<T>(xs: T[:]) { sort(xs) { a, b => a < b } }
fn sort<T, F>(xs: T[:]) {
    let n = xs.len;
    if n <= 10 {
        sort_insertion(xs) { a, b => F(a, b) };
        return;
    }
    let r = sort_run(xs) { a, b => F(a, b) };
    if r == n { return; }
"""
SORT_NEW_HEAD = """fn sort<T>(xs: T[:]) {
    fn less(a: T, b: T) -> bool { a < b }
    fn small(v: T[:]) { sort_presort(v, less); }
    sort_quick(xs, LIMIT, less, small);
}
fn sort<T, F>(xs: T[:]) {
    fn small(v: T[:]) { BLOCK_SMALL }
    sort_quick(xs, BLOCK_LIMIT, F, small);
}
fn sort_quick<T, F, S>(xs: T[:], small: i64) {
    let n = xs.len;
    if n <= small {
        S(xs);
        return;
    }
    let r = sort_run(xs) { a, b => F(a, b) };
    if r == n { return; }
"""
LOOP_OLD = """        if v.len > 10 && limit > 0 {
"""
LOOP_NEW = """        if v.len > small && limit > 0 {
"""
TAIL_OLD = """        if v.len > 10 {
            sort_heap(v) { a, b => F(a, b) };
        } else {
            sort_insertion(v) { a, b => F(a, b) };
        }
"""
TAIL_NEW = """        if v.len > small {
            sort_heap(v) { a, b => F(a, b) };
        } else {
            S(v);
        }
"""
PIVOT_OLD = """fn sort_pivot<T, F>(v: T[:]) -> i64 {
    let n8 = v.len / 8;
    let a = 0;
    let b = n8 * 4;
    let c = n8 * 7;
    if v.len < 128 { return sort_median3(v, a, b, c) { x, y => F(x, y) }; }
    let m8 = n8 / 8;
    let ma = sort_median3(v, a, a + m8 * 4, a + m8 * 7) { x, y => F(x, y) };
    let mb = sort_median3(v, b, b + m8 * 4, b + m8 * 7) { x, y => F(x, y) };
    let mc = sort_median3(v, c, c + m8 * 4, c + m8 * 7) { x, y => F(x, y) };
    return sort_median3(v, ma, mb, mc) { x, y => F(x, y) };
}
"""
PIVOT_NEW = """recursive fn sort_medians<T, F>(v: T[:], a: i64, m: i64) -> i64 {
    var x = a;
    var y = a + m * 4;
    var z = a + m * 7;
    if m >= 8 {
        let n = m / 8;
        x = sort_medians(v, x, n, F);
        y = sort_medians(v, y, n, F);
        z = sort_medians(v, z, n, F);
    }
    return sort_median3(v, x, y, z) { p, q => F(p, q) };
}

fn sort_pivot<T, F>(v: T[:]) -> i64 { sort_medians(v, 0, v.len / 8, F) }
"""


def patch(text, old, new):
    old = old.replace("\n", NL)
    new = new.replace("\n", NL)
    if text.count(old) != 1:
        sys.exit(f"{text.count(old)} matches for {old[:60]!r}")
    return text.replace(old, new)


def small_sort(text, limit, blocks=False):
    head = SORT_NEW_HEAD.replace("LIMIT", str(limit))
    if blocks:
        head = head.replace("BLOCK_SMALL", "sort_presort(v, F);").replace("BLOCK_" + str(limit), str(limit))
        head = head.replace("BLOCK_" + "LIMIT", str(limit)).replace("BLOCK_" + str(limit), str(limit))
    else:
        head = head.replace("BLOCK_SMALL", "sort_insertion(v, F);")
    head = head.replace("sort_quick(xs, BLOCK_" + str(limit) + ", F, small)", "sort_quick(xs, " + (str(limit) if blocks else "10") + ", F, small)")
    text = patch(text, SORT_OLD_HEAD, head)
    text = patch(text, LOOP_OLD, LOOP_NEW)
    text = patch(text, TAIL_OLD, TAIL_NEW)
    anchor = "// Unstable, in place, no allocation: a pattern-defeating quicksort after\n".replace("\n", NL)
    assert text.count(anchor) == 1
    return text.replace(anchor, SMALL.replace("\n", NL) + anchor)


STD, OUT = sys.argv[1], sys.argv[2]
base = open(os.path.join(STD, "std.goose"), encoding="utf-8", newline="").read()
NL = "\r\n" if "\r\n" in base else "\n"
VARIANTS = {
    "main": lambda t: t,
    "pivot": lambda t: patch(t, PIVOT_OLD, PIVOT_NEW),
    "net13": lambda t: small_sort(t, 13),
    "net17": lambda t: small_sort(t, 17),
    "both13": lambda t: small_sort(patch(t, PIVOT_OLD, PIVOT_NEW), 13),
    "both17": lambda t: small_sort(patch(t, PIVOT_OLD, PIVOT_NEW), 17),
    "all17": lambda t: small_sort(patch(t, PIVOT_OLD, PIVOT_NEW), 17, True),
}
for name, fn in VARIANTS.items():
    d = os.path.join(OUT, name)
    shutil.rmtree(d, ignore_errors=True)
    shutil.copytree(STD, d)
    open(os.path.join(d, "std.goose"), "w", encoding="utf-8", newline="").write(fn(base))
print("variants:", " ".join(VARIANTS))
