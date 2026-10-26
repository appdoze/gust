"""Gust: the API example (examples/api/) built normally and with --checked,
each driven over real sockets by examples/api/test_api.py in both of its
ways to serve: the worker pool of event loops (two workers, so that an
abort ends one and the other keeps its connections) and a thread per
connection. It checks every route, the validation errors, the limits,
keep-alive and pipelining, a client that does not read, one that trickles
its request, one that resets mid-response, connection churn, responses
built whole past 16 KB and streamed ones of megabytes, a slow reader of a
stream, fragmented requests, that every socket is closed once its
connection ends, an aborting handler contained to its worker, and a
graceful shutdown with a request and a stream in flight.

In the baseline profile the example is built with `gust build`; in the
sanitize profile its C (`gust -o`) is built with the suite's compiler and
sanitizer flags, and linked with the runtime object built the same way, as
the fixtures are, so that the sanitizers watch the networking runtime
(src/runtime/runtime_net.h) under real traffic."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import toolchain as tc

EXAMPLE = Path(__file__).resolve().parent.parent / "examples" / "api"

# (gust build flags, server arguments)
RUNS = [([], "--workers 2"), (["--checked"], "--workers 2"), ([], "--thread-per-connection")]


def build_example(exe, source, app, flags, env, cc=None, extra=(), runtime=None):
    """Builds source into app: with `gust build`, or, given cc, its C with
    cc and the extra flags, linked with runtime (a GooseRuntime) built the
    same way. Raises RuntimeError on a failure."""
    if cc is None:
        r = subprocess.run([str(exe), "build", *flags, "-o", str(app), str(source)],
                           env=env, capture_output=True, text=True, errors="replace", timeout=600)
        if r.returncode != 0:
            raise RuntimeError(f"gust build {' '.join(flags)} {source.name} failed:\n{r.stdout}{r.stderr}")
        return
    cfile = app.with_suffix(".c")
    r = subprocess.run([str(exe), "-O2", *flags, "-o", str(cfile), str(source)],
                       env=env, capture_output=True, text=True, errors="replace", timeout=600)
    if r.returncode != 0:
        raise RuntimeError(f"gust -o {' '.join(flags)} {source.name} failed:\n{r.stdout}{r.stderr}")
    # --checked is GS_DEBUG=1 in the program's C, and the runtime's.
    defines = ["GS_DEBUG=1"] if "--checked" in flags else []
    ok, log = cc.compile(cfile, app, opt=1, defines=defines, extra=extra, runtime=runtime,
                         log=app.with_suffix(".cc.log"))
    if not ok:
        raise RuntimeError(f"cc {source.name}:\n" + "\n".join(log.splitlines()[:12]))


def check_api_example(exe, base, cc=None, extra=(), runtime=None):
    """Returns why it was skipped, or None when every check ran; raises
    RuntimeError on a failure."""
    base = Path(base).resolve()
    base.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="api-", dir=base) as tmp:
        root = Path(tmp)
        env = dict(os.environ)
        env["GOOSE_HOME"] = str(root / "home")
        built = {}
        for flags, server_args in RUNS:
            key = " ".join(flags)
            if key not in built:
                app = root / ("api" + "".join(f.strip("-") for f in flags) + tc.EXE_SUFFIX)
                build_example(exe, EXAMPLE / "api.gust", app, flags, env, cc, extra, runtime)
                built[key] = app
            r = subprocess.run([sys.executable, str(EXAMPLE / "test_api.py"), "--exe", str(built[key]),
                                "--crash-route", f"--server-args={server_args}"],
                               capture_output=True, text=True, errors="replace", timeout=600)
            if r.returncode != 0:
                failed = [line for line in r.stdout.splitlines() if line.startswith("FAIL")]
                raise RuntimeError(f"api example {key or '(normal build)'} {server_args}:\n"
                                   + "\n".join(failed or r.stdout.splitlines()[-12:]) + r.stderr[-800:])
    return None
