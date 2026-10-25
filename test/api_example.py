"""Gust: the API example (examples/api/) built with `gust build`, normally
and with --checked, each driven over real sockets by
examples/api/test_api.py in both of its ways to serve: the worker pool of
event loops (two workers, so that an abort ends one and the other keeps
its connections) and a thread per connection. It checks every route, the
validation errors, the limits, keep-alive and pipelining, a client that
does not read, one that trickles its request, one that resets mid-response,
connection churn, an aborting handler contained to its worker, and a
graceful shutdown with a request in flight."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import toolchain as tc

EXAMPLE = Path(__file__).resolve().parent.parent / "examples" / "api"

# (gust build flags, server arguments)
RUNS = [([], "--workers 2"), (["--checked"], "--workers 2"), ([], "--thread-per-connection")]


def check_api_example(exe, base):
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
                r = subprocess.run([str(exe), "build", *flags, "-o", str(app), str(EXAMPLE / "api.gust")],
                                   env=env, capture_output=True, text=True, errors="replace", timeout=600)
                if r.returncode != 0:
                    raise RuntimeError(f"api example: gust build {key} failed:\n{r.stdout}{r.stderr}")
                built[key] = app
            r = subprocess.run([sys.executable, str(EXAMPLE / "test_api.py"), "--exe", str(built[key]),
                                "--crash-route", f"--server-args={server_args}"],
                               capture_output=True, text=True, errors="replace", timeout=300)
            if r.returncode != 0:
                failed = [line for line in r.stdout.splitlines() if line.startswith("FAIL")]
                raise RuntimeError(f"api example {key or '(normal build)'} {server_args}:\n"
                                   + "\n".join(failed or r.stdout.splitlines()[-12:]) + r.stderr[-800:])
    return None
