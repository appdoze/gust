#!/usr/bin/env python3
"""Gust: fuzzing the compiler for crashes. Programs from the test fixtures
and samples, mutated at random by tokens and by lines (a token deleted,
doubled, swapped with its neighbour or replaced by another from the same
file, a number set to a boundary, a keyword or punctuation inserted, a line
deleted, doubled or moved), each checked with `gust check`. A clean result
and a reported error are both fine; anything else is kept as a finding:
an exit other than 0 or 1 (a crash, an abort, an access violation), a
message the compiler says is internal, or no answer within the timeout.

A program that checks clean also goes through code generation at -O2, and
its C through a C compiler's -fsyntax-only, so that the optimizer, the
bounds-check pass and the code generator are fuzzed too.

  python bench/compiler_fuzz.py [--exe build/Release/gust.exe] [--seed 1] [--count 2000]
                                [--jobs 8] [--cc auto|none|path] [--out build/gen/compiler_fuzz]
"""
import argparse, concurrent.futures, os, random, re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|[A-Za-z_][A-Za-z0-9_]*|0x[0-9a-fA-F_]+|'
                   r'[0-9][0-9_]*(?:\.[0-9_]+)?(?:[eE][-+]?[0-9]+)?|\.\.<|\.\.=|\.\.|[-+*/%&|^!<>=]=|'
                   r'&&|\|\||<<|>>|->|=>|::|\.=|\S')
INSERTS = ["fn", "let", "var", "if", "else", "while", "for", "in", "match", "return", "break",
           "continue", "struct", "enum", "recursive", "export", "extern", "self", "null", "true",
           "{", "}", "(", ")", "[", "]", "<", ">", ",", ";", ":", "=", ".", "..", "&", "?", "@",
           "u8", "i64", "f64", "T", "[>..]", "[>..<]", "[..4]", "&<u16>", "..&", "as", "as!",
           "0", "-1", "9223372036854775807", "18446744073709551615", "1e308", "0.0"]
NUMBERS = ["0", "1", "-1", "255", "256", "65535", "2147483647", "-2147483648", "9223372036854775807",
           "-9223372036854775808", "18446744073709551616", "1e309", "0x7fffffffffffffff", "3.5"]


TYPES = ["i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "f32", "f64", "bool", "varint"]
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*$")


def mutate(r, text):
    toks = [m for m in TOKEN.finditer(text)]
    if not toks:
        return text
    # Most of the time a replacement in kind, which usually still parses and
    # so reaches the checker: an identifier for another of the file's, a
    # number for another, a type for another.
    if r.random() < 0.6:
        idents = [m for m in toks if IDENT.match(m.group(0)) and m.group(0) not in INSERTS]
        nums = [m for m in toks if m.group(0)[:1].isdigit()]
        types = [m for m in toks if m.group(0) in TYPES]
        pool = r.choice([x for x in (idents, nums, types) if x] or [toks])
        m = r.choice(pool)
        a, b = m.span()
        if pool is nums:
            new = r.choice(NUMBERS)
        elif pool is types:
            new = r.choice(TYPES)
        else:
            new = r.choice(pool).group(0)
        return text[:a] + new + text[b:]
    lines = text.split("\n")
    kind = r.randrange(9)
    if kind >= 7 and len(lines) > 2:
        i = r.randrange(len(lines))
        if kind == 7:
            del lines[i]
        else:
            j = r.randrange(len(lines))
            lines.insert(j, lines[i])
        return "\n".join(lines)
    i = r.randrange(len(toks))
    a, b = toks[i].span()
    tok = toks[i].group(0)
    if kind == 0:
        return text[:a] + text[b:]
    if kind == 1:
        return text[:a] + tok + " " + tok + text[b:]
    if kind == 2 and i + 1 < len(toks):
        c, d = toks[i + 1].span()
        return text[:a] + text[c:d] + text[b:c] + tok + text[d:]
    if kind == 3:
        return text[:a] + r.choice(toks).group(0) + text[b:]
    if kind == 4 and tok[:1].isdigit():
        return text[:a] + r.choice(NUMBERS) + text[b:]
    return text[:a] + r.choice(INSERTS) + " " + text[a:]


# A C compiler for the generated C, where there is one (clang first, as
# the toolchain picks; --cc names another or "none").
CC = None


def check(exe, path, timeout):
    try:
        p = subprocess.run([exe, "check", "--offline", str(path)], capture_output=True, text=True, errors="replace",
                           timeout=timeout)
    except subprocess.TimeoutExpired:
        return "no answer in time", ""
    text = p.stdout + p.stderr
    if p.returncode not in (0, 1):
        return f"exit {p.returncode}", text
    # The compiler's own markers: Fail("internal: ...") and a C assert.
    if re.search(r"error: internal|internal: |Assertion failed", text):
        return "internal error", text
    # How far it got, for the totals: checked clean, or an error after the
    # parse (a type error) or in it.
    if p.returncode == 0:
        # The rest of the pipeline too: the optimizer, bounds-check
        # elimination and code generation.
        cfile = path.with_suffix(".c")
        try:
            q = subprocess.run([exe, "-O2", "--offline", "-q", "-o", str(cfile), str(path)], capture_output=True,
                               text=True, errors="replace", timeout=timeout)
        except subprocess.TimeoutExpired:
            cfile.unlink(missing_ok=True)
            return "no answer in time (generating C)", ""
        said = q.stdout + q.stderr
        if q.returncode not in (0, 1):
            cfile.unlink(missing_ok=True)
            return f"exit {q.returncode} generating C", said
        if re.search(r"error: internal|internal: |Assertion failed", said):
            cfile.unlink(missing_ok=True)
            return "internal error generating C", said
        # And the C must compile: a C compiler's error is the code
        # generator's (a main whose C no main() can call, say).
        if q.returncode == 0 and CC:
            try:
                c = subprocess.run([CC, "-fsyntax-only", "-w", str(cfile)], capture_output=True, text=True,
                                   errors="replace", timeout=timeout)
                # An extern whose Goose signature a mutation changed from
                # its C declaration's (cbrt, a header's own function) is
                # the program's error, which C reports as it should.
                conflict = re.search(r"conflicting types|previous declaration is here|redefinition of", c.stderr)
                if c.returncode != 0 and not conflict:
                    cfile.unlink(missing_ok=True)
                    return "the generated C does not compile", c.stderr
            except subprocess.TimeoutExpired:
                pass
        cfile.unlink(missing_ok=True)
        return None, "clean"
    return None, "typecheck" if "parsed ok" in text else "parse"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(ROOT / "build" / "Release" / ("gust.exe" if os.name == "nt" else "gust")))
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=2000)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--timeout", type=float, default=20)
    ap.add_argument("--out", default=str(ROOT / "build" / "gen" / "compiler_fuzz"))
    ap.add_argument("--cc", default="auto", help="the C compiler for -fsyntax-only, or none")
    args = ap.parse_args()
    global CC
    if args.cc == "auto":
        import shutil
        CC = shutil.which("clang") or next((p for p in (r"C:\Program Files\LLVM\bin\clang.exe",)
                                            if os.path.exists(p)), None)
    elif args.cc != "none":
        CC = args.cc
    exe = str(Path(args.exe).resolve())
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    # Fixtures that import siblings or need a directory of their own are
    # mutated in place beside their originals' directory, so imports resolve.
    corpus = sorted(p for p in list(ROOT.glob("test/*/*.goose")) + list(ROOT.glob("samples/*.goose"))
                    if p.stat().st_size < 40000)
    r = random.Random(args.seed)
    plans = []
    for k in range(args.count):
        src = r.choice(corpus)
        text = src.read_text(encoding="utf-8", errors="replace")
        for _ in range(r.randint(1, 3)):
            text = mutate(r, text)
        plans.append((k, src, text))

    def one(plan):
        k, src, text = plan
        path = src.parent / f"zz_fuzz_{os.getpid()}_{k}.goose"
        path.write_text(text, encoding="utf-8", newline="\n")
        try:
            why, said = check(exe, path, args.timeout)
        finally:
            path.unlink(missing_ok=True)
        if not why:
            depth[said] = depth.get(said, 0) + 1
        if why:
            keep = out / f"crash_{args.seed}_{k}_{src.stem}.goose"
            keep.write_text(text, encoding="utf-8", newline="\n")
            return f"{why}: {keep} (from {src.relative_to(ROOT)})\n    " + "\n    ".join(said.strip().splitlines()[-4:])
        return None

    found = []
    depth = {}
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for res in pool.map(one, plans):
            if res:
                found.append(res)
                print(res, flush=True)
    print(f"compiler_fuzz: {len(plans)} programs ({depth.get('clean', 0)} checked clean, "
          f"{depth.get('typecheck', 0)} rejected after the parse, {depth.get('parse', 0)} by it), "
          f"{len(found)} finding(s)")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
