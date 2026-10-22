#!/usr/bin/env python3
# Modified for Gust (https://github.com/appdoze/gust); see MODIFICATIONS.md.
"""Compile every ```goose block of a markdown document with the compiler.

    python scripts/check_doc_examples.py docs/reference.md [--run] [--exe build/goose] [-v]

A block compiles as written or, failing that, wrapped: `import std;` in
front, the block inside `fn main() { }`, or an empty `fn main() {}` after
it, so a document can show the body of a main or a set of declarations
without the scaffolding. A block holding a live line (not a comment) that
ends in a `// error` note must fail to compile in every form. A warning
from the compiler fails the block too: a document's examples print none.
An HTML comment on the line before a fence marks the exceptions:
`<!-- fragment -->` skips a block that is not meant to compile on its own,
and `<!-- check-only -->` compiles a program without running it (one with
threads, which the in-process backend refuses).

With --run, each complete program is also run in-process, and its output
compared with a bare ``` block that directly follows it, as the reference
shows its examples' output. The compiler is `--exe`, else the first of
build/Release, build/RelWithDebInfo, build and build/Debug that holds one,
as the test runners choose. The blocks are checked --jobs at a time, and
the report is in document order whatever the number of jobs: a FAIL line
per failing block with the compiler's diagnostic or the differing output,
and a summary line last; -v adds a line for every block that passed, the
diagnostic each expected error produced, and the output of a program the
document shows no output for. The examples are written under --out-dir
(default build/doc_examples). test/run_tests.py runs this over the
language reference.
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_compiler():
    suffix = ".exe" if sys.platform == "win32" else ""
    for d in ("build/Release", "build/RelWithDebInfo", "build", "build/Debug"):
        for name in ("gust", "goose"):
            p = os.path.join(ROOT, d, name + suffix)
            if os.path.exists(p):
                return p
    raise SystemExit("no compiler found under build/; pass --exe")


def read_blocks(path):
    """The ```goose blocks: (first line number, lines, expected output, marker)."""
    lines = open(path, encoding="utf-8").read().split("\n")
    blocks = []
    i = 0
    while i < len(lines):
        m = re.match(r"^```(\w*)", lines[i])
        if not (m and m.group(1) == "goose"):
            i += 1
            continue
        marker = ""
        if i > 0:
            mm = re.match(r"^<!--\s*(fragment|check-only)\s*-->", lines[i - 1])
            if mm:
                marker = mm.group(1)
        start = i + 1
        body = []
        i += 1
        while i < len(lines) and not lines[i].startswith("```"):
            body.append(lines[i])
            i += 1
        i += 1
        # A bare ``` block right after the program is its expected output.
        expected = None
        j = i
        while j < len(lines) and lines[j].strip() == "":
            j += 1
        if j < len(lines) and lines[j].strip() == "```":
            expected = []
            j += 1
            while j < len(lines) and not lines[j].startswith("```"):
                expected.append(lines[j])
                j += 1
        blocks.append((start, body, expected, marker))
    return blocks


class Checker:
    def __init__(self, goose, out_dir, run, verbose):
        self.goose, self.out_dir, self.run, self.verbose = goose, out_dir, run, verbose

    def compile_text(self, text, name):
        path = os.path.join(self.out_dir, name + ".goose")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        p = subprocess.run([self.goose, "check", path], capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        msg = "\n".join(l for l in (p.stdout + p.stderr).split("\n")
                        if not l.startswith(("parsed ok", "typechecked ok", "bce:")))
        return p.returncode, msg.strip(), path

    def check(self, block):
        """One block's report: (lines to print, failures)."""
        start, body, expected, marker = block
        text = "\n".join(body) + "\n"
        expect_error = any(not l.lstrip().startswith("//") and re.search(r"//\s*error", l) for l in body)
        has_main = re.search(r"^\s*fn main\(", text, re.M) is not None
        forms = [("as-is", text), ("std", "import std;\n" + text)]
        if not has_main:
            forms += [("main-wrap", "import std;\nfn main() {\n" + text + "}\n"),
                      ("main-after", "import std;\n" + text + "\nfn main() {}\n")]
        lines = []
        results = []
        for label, t in forms:
            rc, msg, path = self.compile_text(t, "ex_%04d_%s" % (start, label))
            results.append((label, rc, msg, path))
            if rc == 0 and not expect_error:
                break
        ok_forms = [r for r in results if r[1] == 0]
        if expect_error:
            if ok_forms:
                lines.append("FAIL line %d: expected an error, but it compiled as %s" % (start, ok_forms[0][0]))
                return lines, 1
            if self.verbose:
                best = next((r for r in results if "error" in r[2]), results[-1])
                first = [l for l in best[2].split("\n") if "error" in l or "warning" in l][:2]
                lines.append("ok   line %d (expected error, %s): %s" % (start, best[0], " | ".join(first)))
            return lines, 0
        if not ok_forms:
            lines.append("FAIL line %d: no form compiles" % start)
            for label, rc, msg, path in results:
                first = [l for l in msg.split("\n") if l.strip()][:3]
                lines.append("   [%s] %s" % (label, " | ".join(first)))
            return lines, 1
        label, rc, msg, path = ok_forms[0]
        warn = [l for l in msg.split("\n") if "warning" in l]
        if warn:
            lines.append("FAIL line %d (%s): the compiler warns: %s" % (start, label, " | ".join(warn[:2])))
            return lines, 1
        if self.run and label in ("as-is", "std") and has_main and marker != "check-only":
            p = subprocess.run([self.goose, path], capture_output=True, text=True, encoding="utf-8",
                               errors="replace", timeout=300)
            if p.returncode != 0:
                tail = p.stderr.strip().split("\n")[-1] if p.stderr.strip() else p.stdout.strip()[-200:]
                lines.append("FAIL line %d: the program exited %d: %s" % (start, p.returncode, tail))
                return lines, 1
            if expected is not None:
                got = p.stdout.replace("\r\n", "\n").rstrip("\n").split("\n")
                exp = "\n".join(expected).rstrip("\n").split("\n")
                if got != exp:
                    lines.append("FAIL line %d: the output differs from the block shown" % start)
                    lines.append("   expected: " + " / ".join(exp)[:400])
                    lines.append("   got:      " + " / ".join(got)[:400])
                    return lines, 1
                if self.verbose:
                    lines.append("ok   line %d (%s, ran, output as shown)" % (start, label))
            elif self.verbose:
                lines.append("ok   line %d (%s, ran):" % (start, label))
                lines += ["   " + l for l in p.stdout.rstrip().split("\n")]
            return lines, 0
        if self.verbose:
            lines.append("ok   line %d (%s)" % (start, label))
        return lines, 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("markdown")
    ap.add_argument("--exe", help="the compiler (default: the newest build under build/)")
    ap.add_argument("--run", action="store_true", help="also run complete programs and compare their output")
    ap.add_argument("--out-dir", help="where the examples are written (default build/doc_examples)")
    ap.add_argument("-j", "--jobs", type=int, default=min(os.cpu_count() or 1, 32),
                    help="how many blocks are checked at once")
    ap.add_argument("--line", type=int, help="check only the block starting at this line")
    ap.add_argument("-v", "--verbose", action="store_true", help="a line for every block, not only the failures")
    a = ap.parse_args()
    goose = os.path.abspath(a.exe) if a.exe else find_compiler()
    out_dir = os.path.abspath(a.out_dir) if a.out_dir else os.path.join(ROOT, "build", "doc_examples")
    os.makedirs(out_dir, exist_ok=True)
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    blocks = read_blocks(a.markdown)
    if a.line is not None:
        blocks = [b for b in blocks if b[0] == a.line]
    fragments = [b for b in blocks if b[3] == "fragment"]
    todo = [b for b in blocks if b[3] != "fragment"]
    checker = Checker(goose, out_dir, a.run, a.verbose)
    if a.jobs > 1 and len(todo) > 1:
        with concurrent.futures.ThreadPoolExecutor(min(a.jobs, len(todo))) as pool:
            reports = list(pool.map(checker.check, todo))
    else:
        reports = [checker.check(b) for b in todo]
    failures = 0
    for lines, failed in reports:
        failures += failed
        for line in lines:
            print(line)
    ran = sum(1 for b in todo if a.run and b[3] != "check-only"
              and re.search(r"^\s*fn main\(", "\n".join(b[1]), re.M))
    shown = os.path.relpath(a.markdown, ROOT).replace(os.sep, "/")
    if shown.startswith(".."):
        shown = a.markdown
    print("%s: %d blocks, %d checked%s, %d fragments skipped, %d failures" %
          (shown, len(blocks), len(todo), (", %d run" % ran) if a.run else "", len(fragments), failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
