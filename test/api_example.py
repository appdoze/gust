"""Gust: the API example (examples/api/) built with `gust build`, normally
and with --checked, and each build driven over real sockets by
examples/api/test_api.py: every route, the validation errors, the limits,
keep-alive and pipelining, an aborting handler contained to its connection,
and a graceful shutdown with a connection open."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import toolchain as tc

EXAMPLE = Path(__file__).resolve().parent.parent / "examples" / "api"


def check_api_example(exe, base):
    """Returns why it was skipped, or None when every check ran; raises
    RuntimeError on a failure."""
    base = Path(base).resolve()
    base.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="api-", dir=base) as tmp:
        root = Path(tmp)
        env = dict(os.environ)
        env["GOOSE_HOME"] = str(root / "home")
        for flags in ([], ["--checked"]):
            app = root / ("api" + "".join(f.strip("-") for f in flags) + tc.EXE_SUFFIX)
            r = subprocess.run([str(exe), "build", *flags, "-o", str(app), str(EXAMPLE / "api.gust")],
                               env=env, capture_output=True, text=True, errors="replace", timeout=600)
            if r.returncode != 0:
                raise RuntimeError(f"api example: gust build {' '.join(flags)} failed:\n{r.stdout}{r.stderr}")
            r = subprocess.run([sys.executable, str(EXAMPLE / "test_api.py"), "--exe", str(app), "--crash-route"],
                               capture_output=True, text=True, errors="replace", timeout=300)
            if r.returncode != 0:
                failed = [line for line in r.stdout.splitlines() if line.startswith("FAIL")]
                raise RuntimeError(f"api example {' '.join(flags) or '(normal build)'}:\n"
                                   + "\n".join(failed or r.stdout.splitlines()[-12:]) + r.stderr[-800:])
    return None
