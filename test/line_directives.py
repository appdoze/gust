"""Gust: a --debug build's C carries a `#line` before each statement whose
source line changed, naming the .gust file, so that debuggers, profilers
and sanitizer reports point at the program's own lines; a release build's
C carries none, and the debug build still compiles and runs."""
import re
import subprocess
import tempfile
from pathlib import Path

import toolchain as tc

PROGRAM = """fn twice(x: i64) -> i64 {
    let y = x * 2;
    return y;
}

fn main() {
    var total = 0;
    for i in 3 {
        total += twice(i);
    }
    print("total ", total);
}
"""


def check_line_directives(exe, base, cc=None):
    """Raises RuntimeError on a failure; returns None."""
    base = Path(base).resolve()
    base.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="lines-", dir=base) as tmp:
        root = Path(tmp)
        src = root / "lines.gust"
        src.write_text(PROGRAM, encoding="utf-8")
        for flags, want in ((["--debug"], True), ([], False)):
            cfile = root / ("debug.c" if want else "release.c")
            r = subprocess.run([str(exe), "-q", "-O0", *flags, "-o", str(cfile), str(src)],
                               capture_output=True, text=True, errors="replace", timeout=120)
            if r.returncode:
                raise RuntimeError(f"line directives: gust -o {' '.join(flags)}:\n{r.stdout}{r.stderr}")
            text = cfile.read_text(encoding="utf-8", errors="replace")
            lines = {int(m.group(1)) for m in re.finditer(r'^\s*#line (\d+) "[^"]*lines\.gust"', text, re.M)}
            if want and not {2, 3, 7, 9, 11} <= lines:
                raise RuntimeError(f"line directives: --debug C names lines {sorted(lines)} of lines.gust; "
                                   "2, 3, 7, 9 and 11 are statements")
            if not want and "#line" in text:
                raise RuntimeError("line directives: a release build's C has #line directives")
        if cc is None:
            return None
        app = root / ("lines" + tc.EXE_SUFFIX)
        r = subprocess.run([str(exe), "build", "-q", "--debug", "-o", str(app), str(src)],
                           capture_output=True, text=True, errors="replace", timeout=600)
        if r.returncode:
            raise RuntimeError(f"line directives: gust build --debug:\n{r.stdout}{r.stderr}")
        out = subprocess.run([str(app)], capture_output=True, text=True, timeout=60).stdout
        if out.strip() != "total 6":
            raise RuntimeError(f"line directives: the --debug build printed {out!r}")
    return None
