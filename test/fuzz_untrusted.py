"""Gust: long fuzzing runs of the code that reads untrusted input, beyond
the suite's short fixed-seed runs of the same programs:

- test/storage/fuzz_from_bytes.goose, from_bytes on mutated images;
- test/stdlib/fuzz_json.goose, the JSON parser and reader on mutated text;
- examples/api/fuzz_http.py, the API server on mutated requests.

On Linux and macOS each is built from its standalone C with clang (or cc)
under AddressSanitizer and UndefinedBehaviorSanitizer, once as is and once
with the --checked runtime checks; on Windows with `gust build`, normal and
--checked. A sanitizer report, an abort or a failed check fails the run.

    python test/fuzz_untrusted.py [--exe build/gust] [--count 100000] [--seeds 4] [--http 50000]
"""
import argparse, os, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROGRAMS = [ROOT / "test" / "storage" / "fuzz_from_bytes.goose", ROOT / "test" / "stdlib" / "fuzz_json.goose"]
API = ROOT / "examples" / "api" / "api.gust"
# As the suite's sanitize profile (scripts/toolchain.py): packed fields use
# unaligned scalar accesses on purpose.
SANITIZE = ["-fsanitize=address,undefined", "-fno-sanitize=alignment", "-fno-sanitize-recover=all",
            "-fno-omit-frame-pointer"]


def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, errors="replace", **kw)
    return r.returncode, r.stdout, r.stderr


def build(exe, source, out, checked, cc, env):
    """source built into out; raises RuntimeError on a failure."""
    flags = ["--checked"] if checked else []
    if cc is None:
        code, o, e = run([exe, "build", *flags, "-q", "-o", out, source], env=env, timeout=900)
        if code:
            raise RuntimeError(f"gust build {source.name}: {o}{e}")
        return
    cfile = out.with_suffix(".c")
    code, o, e = run([exe, "-O2", *flags, "--standalone", "-q", "-o", cfile, source], env=env, timeout=900)
    if code:
        raise RuntimeError(f"gust -o {source.name}: {o}{e}")
    defines = ["-DGS_DEBUG=1"] if checked else []
    code, o, e = run([cc, "-O1", "-g", *SANITIZE, *defines, "-w", "-o", out, cfile, "-lpthread", "-lm"],
                     timeout=900)
    if code:
        raise RuntimeError(f"{cc} {cfile.name}: {e[-2000:]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(ROOT / "build" / ("Release/gust.exe" if os.name == "nt" else "gust")))
    ap.add_argument("--count", type=int, default=100000, help="inputs per shape or document, per seed")
    ap.add_argument("--seeds", type=int, default=4)
    ap.add_argument("--http", type=int, default=50000, help="mutated requests per server build")
    args = ap.parse_args()
    cc = None if os.name == "nt" else shutil.which("clang") or shutil.which("cc")
    env = dict(os.environ)
    # Leaks at exit are the runtime's arenas, freed by the OS; the reports
    # wanted here are the out-of-bounds and undefined ones.
    env["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=0"
    env["UBSAN_OPTIONS"] = "print_stacktrace=1:halt_on_error=1"
    failures = []
    with tempfile.TemporaryDirectory(prefix="fuzz-") as tmp:
        tmp = Path(tmp)
        env["GOOSE_HOME"] = str(tmp / "home")
        suffix = ".exe" if os.name == "nt" else ""
        for checked in (False, True):
            label = ("checked" if checked else "normal") + (", ASan+UBSan" if cc else "")
            for prog in PROGRAMS:
                out = tmp / (prog.stem + ("_checked" if checked else "") + suffix)
                build(args.exe, prog, out, checked, cc, env)
                for seed in range(1, args.seeds + 1):
                    code, o, e = run([out, args.count, seed], env=env, timeout=3600)
                    bad = code != 0 or "Sanitizer" in e or "runtime error:" in e
                    print(f"{'FAIL' if bad else 'ok  '} {prog.stem} ({label}) seed {seed}:",
                          " / ".join(o.strip().splitlines()[-10:]), flush=True)
                    if bad:
                        failures.append(f"{prog.stem} ({label}) seed {seed}: exit {code}\n{o[-1500:]}{e[-3000:]}")
            api = tmp / ("api" + ("_checked" if checked else "") + suffix)
            build(args.exe, API, api, checked, cc, env)
            code, o, e = run([sys.executable, ROOT / "examples" / "api" / "fuzz_http.py", "--exe", api,
                              "--cases", args.http, "--server-args=--workers 4"], env=env, timeout=3600)
            print(f"{'FAIL' if code else 'ok  '} fuzz_http ({label}):", " / ".join(o.strip().splitlines()[-4:]),
                  flush=True)
            if code:
                failures.append(f"fuzz_http ({label}):\n{o[-3000:]}{e[-1000:]}")
    for f in failures:
        print("\n" + f)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
