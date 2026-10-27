"""Gust: in-process runs through MIR (`gust run --mir`, src/jit_mir.h) against
TinyCC's (`gust run`). Each fixture of the categories below that has an
expected output is run both ways, and must print the same and end the same
way (an exit status of 0, or not) under MIR as under TinyCC: --mir changes
how fast a run is, never what it does. On every platform: MIR compiles the
program's own C against the runtime compiled into the compiler
(src/runtime_native.c)."""
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
CATEGORIES = ("codegen", "optimizer", "lifetimes", "stdlib", "storage", "typing")


def check_mir_runs(exe, jobs=1):
    """The number of fixtures compared; raises RuntimeError on a difference."""
    cases = [f for cat in CATEGORIES for f in sorted((HERE / cat).glob("*.goose"))
             if (HERE / "expected" / f"{f.stem}.out").exists()]

    def run(f, mir):
        try:
            p = subprocess.run([str(exe), "run", "-q", *(["--mir"] if mir else []), str(f)], cwd=f.parent,
                               capture_output=True, text=True, errors="replace", timeout=300)
            return p.returncode, p.stdout.replace("\r\n", "\n"), p.stderr
        except subprocess.TimeoutExpired:
            return "timeout", "", ""

    def one(f):
        tcode, tout, _ = run(f, False)
        mcode, mout, merr = run(f, True)
        if mout != tout or (mcode == 0) != (tcode == 0):
            return (f"{f.parent.name}/{f.name}: exit {mcode} under MIR, {tcode} under TinyCC\n--- MIR:\n"
                    f"{mout[-500:]}\n--- TinyCC:\n{tout[-500:]}\n--- MIR stderr:\n{merr[:1500]}")
        return None

    with ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        problems = [r for r in pool.map(one, cases) if r]
    if problems:
        raise RuntimeError(f"{len(problems)} of {len(cases)} fixture(s) differ under --mir:\n" +
                           "\n".join(problems[:5]))
    return len(cases)
