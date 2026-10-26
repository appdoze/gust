"""Compile and run every safety probe in compare/safety/<lang>/ and record what
happened. Writes results.json beside this script (or --out).

    python compare/safety/run_safety.py [--out path]
    python compare/safety/run_safety.py --check

Each probe deliberately contains one classic bug. A probe either fails to
compile (the language caught it statically), compiles and stops with a
run-time error (caught dynamically), or compiles and runs to the end, in
which case its output says whether the result was well defined.

--check is what CI runs: the Gust probes only, in each of Gust's modes
(release, --checked, --debug), each held to what expected.json says it
does; any difference fails the run. A probe whose expectation records a
bug Gust does not catch yet says so in its "note", so that fixing one
shows as a failure here until expected.json follows.

Rust runs where rustc is installed and Lobster where LOBSTER (lobster.exe,
or its repository root) is set; neither is checked.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(HERE, "out")
EXE = ".exe" if os.name == "nt" else ""


def scrub(text):
    """text with this machine's home directory and the repository's path
    replaced, so that a recorded result names no local paths."""
    for path, name in ((ROOT, "<repo>"), (os.path.expanduser("~"), "~")):
        for form in {path, path.replace("\\", "/"), path.replace("/", "\\")}:
            text = text.replace(form, name)
    return text


def scrub_all(v):
    """scrub over every string in a result."""
    if isinstance(v, str):
        return scrub(v)
    if isinstance(v, dict):
        return {k: scrub_all(x) for k, x in v.items()}
    if isinstance(v, list):
        return [scrub_all(x) for x in v]
    return v


def find_gust():
    env = os.environ.get("GUST")
    if env:
        # A path is made absolute here: the builds run in other directories,
        # where a relative one would name nothing (POSIX resolves a program's
        # path against the directory it starts in).
        return os.path.abspath(env) if "/" in env or os.sep in env else env
    for d in ("build/Release", "build/RelWithDebInfo", "build", "build/Debug"):
        p = os.path.join(ROOT, d, "gust" + EXE)
        if os.path.exists(p):
            return p
    sys.exit("no gust under build/; set GUST")


def run(cmd, cwd=HERE, timeout=120):
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout,
                           encoding="utf-8", errors="replace")
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return "timeout", "", ""


def short(text, n=400):
    text = text.strip()
    return text if len(text) <= n else text[:n] + " ..."


def probe_gust(path, name, mode, timeout=120):
    exe = os.path.join(OUT, f"gust_{mode}_{name}{EXE}")
    flags = {"release": [], "checked": ["--checked"], "debug": ["--debug"]}[mode]
    code, out, err = run([GUST, "build", "-q", *flags, path, "-o", exe], timeout=300)
    if code != 0:
        text = err or out
        first = next((l for l in text.splitlines() if "error" in l), text)
        return {"stage": "compile", "exit": code, "message": short(first)}
    code, out, err = run([exe], timeout=timeout)
    return {"stage": "run", "exit": code, "stdout": short(out), "stderr": short(err)}


def probe_rust(path, name, mode):
    exe = os.path.join(OUT, f"rust_{mode}_{name}{EXE}")
    # rustc with no -O is a debug build: overflow checks and debug assertions on.
    flags = ["-C", "opt-level=3"] if mode == "release" else []
    code, out, err = run(["rustc", *flags, "-C", "debuginfo=0", "-o", exe, path])
    if code != 0:
        first = next((l for l in err.splitlines() if l.startswith("error")), err)
        return {"stage": "compile", "exit": code, "message": short(first)}
    code, out, err = run([exe])
    return {"stage": "run", "exit": code, "stdout": short(out), "stderr": short(err)}


def probe_lobster(path, name, mode):
    # Lobster compiles and runs in one process: a compile error names a file
    # and a line and says "error". It has one checked mode, asserts on.
    exe = os.environ["LOBSTER"]
    root = exe if os.path.isdir(exe) else os.path.dirname(os.path.dirname(os.path.abspath(exe)))
    if os.path.isdir(exe):
        exe = os.path.join(exe, "bin", "lobster" + EXE)
    code, out, err = run([exe, path, "--"], cwd=root)
    return {"stage": "compile+run", "exit": code, "output": short((out + "\n" + err).strip(), 600)}


def outcome(r):
    """What a Gust probe did: compile_error, abort (a runtime error), ok, or
    timeout (still running when its time was up)."""
    if r["stage"] == "compile":
        return "compile_error"
    if r["exit"] == "timeout":
        return "timeout"
    if r["exit"] == 0:
        return "ok"
    return "abort" if "runtime error" in r.get("stderr", "") else f"exit {r['exit']}"


def meets(r, want):
    """Whether result r is what expectation want says; why not, if it is not."""
    got = outcome(r)
    allowed = want["outcome"] if isinstance(want["outcome"], list) else [want["outcome"]]
    if got not in allowed:
        return f"{got} where {want['outcome']} was expected: {r.get('message') or r.get('stderr') or r.get('stdout')}"
    text = r.get("message", "") + r.get("stderr", "") + r.get("stdout", "")
    for part in want.get("contains", []) if got == allowed[0] else []:
        if part not in text:
            return f"{got}, but without {part!r}: {short(text, 200)}"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(HERE, "results.json"))
    ap.add_argument("--check", action="store_true", help="the Gust probes against expected.json (CI)")
    args = ap.parse_args()
    global GUST
    GUST = find_gust()
    os.makedirs(OUT, exist_ok=True)
    langs = {"gust": (".goose", probe_gust, ["release", "checked", "debug"])}
    if not args.check:
        if shutil.which("rustc"):
            langs["rust"] = (".rs", probe_rust, ["release", "debug"])
        if os.environ.get("LOBSTER"):
            langs["lobster"] = (".lobster", probe_lobster, ["default"])
    expected = json.load(open(os.path.join(HERE, "expected.json"), encoding="utf-8"))
    results, failures = {}, []
    for lang, (ext, fn, modes) in langs.items():
        d = os.path.join(HERE, lang)
        for f in sorted(os.listdir(d)):
            if not f.endswith(ext):
                continue
            name = f[: -len(ext)]
            for mode in modes:
                limit = expected.get(name, {}).get("timeout")
                if lang == "gust" and limit:
                    r = fn(os.path.join(d, f), name, mode, limit)
                else:
                    r = fn(os.path.join(d, f), name, mode)
                results.setdefault(name, {})[f"{lang}/{mode}"] = r
                status = ""
                if lang == "gust":
                    want = expected.get(name, {}).get(mode)
                    why = meets(r, want) if want else "no expectation in expected.json"
                    status = "ok   " if why is None else "FAIL "
                    if why:
                        failures.append(f"{name} {mode}: {why}")
                print(f"{status}{lang}/{mode} {name}: {outcome(r) if lang == 'gust' else r['stage']}", flush=True)
    if not args.check:
        with open(args.out, "w") as f:
            json.dump(scrub_all(results), f, indent=2)
        print("wrote", args.out)
    for line in failures:
        print("FAIL", line)
    print(f"{len(failures)} probe result(s) differ from expected.json" if failures else
          "every Gust probe did what expected.json says")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
