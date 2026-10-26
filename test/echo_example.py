"""Gust: the TCP echo example (examples/echo/), built normally and with
--checked as api_example.py builds the API example (in the sanitize
profile, with the suite's sanitizer flags). Each build runs its own --check,
which serves itself and checks interleaved clients, a client that reads
nothing at first, and churn; then it serves on a port of its own while this
process connects as clients do, and stops on Ctrl-C's signal with the
totals it served."""
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import toolchain as tc
from api_example import build_example

EXAMPLE = Path(__file__).resolve().parent.parent / "examples" / "echo"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def echo_back(port, data):
    """Sends data on a connection while reading its echo: what came back."""
    with socket.create_connection(("127.0.0.1", port)) as s:
        sender = threading.Thread(target=s.sendall, args=(data,), daemon=True)
        sender.start()
        got = []
        n = 0
        while n < len(data):
            d = s.recv(65536)
            if not d:
                break
            got.append(d)
            n += len(d)
        sender.join(10)
        return b"".join(got)


def serve_and_stop(app, label):
    port = free_port()
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    proc = subprocess.Popen([str(app), str(port), "--workers", "2"], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, creationflags=flags)
    try:
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.05)
        data = bytes((i * 13 + i // 1000) & 255 for i in range(3 << 20))
        if echo_back(port, data) != data:
            raise RuntimeError(f"echo example {label}: 3 MB did not come back intact")
        for k in range(50):
            msg = b"line %d\n" % k
            if echo_back(port, msg) != msg:
                raise RuntimeError(f"echo example {label}: connection {k} did not echo")
    finally:
        proc.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGINT)
        try:
            out, err = proc.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, err = proc.communicate()
            raise RuntimeError(f"echo example {label}: did not stop on the signal")
    text = out.decode(errors="replace")
    if proc.returncode != 0 or "echoed" not in text or err.strip():
        raise RuntimeError(f"echo example {label}: exit {proc.returncode}\n{text}{err.decode(errors='replace')}")


def check_echo_example(exe, base, cc=None, extra=(), runtime=None):
    """Raises RuntimeError on a failure."""
    base = Path(base).resolve()
    base.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="echo-", dir=base) as tmp:
        root = Path(tmp)
        env = dict(os.environ)
        env["GOOSE_HOME"] = str(root / "home")
        for flags in ([], ["--checked"]):
            label = " ".join(flags) or "(normal build)"
            app = root / ("echo" + "".join(f.strip("-") for f in flags) + tc.EXE_SUFFIX)
            build_example(exe, EXAMPLE / "echo.gust", app, flags, env, cc, extra, runtime)
            r = subprocess.run([str(app), "--check"], capture_output=True, text=True, errors="replace",
                               timeout=300)
            if r.returncode != 0 or "all checks passed" not in r.stdout or r.stderr.strip():
                raise RuntimeError(f"echo example {label} --check: exit {r.returncode}\n{r.stdout}{r.stderr}")
            serve_and_stop(app, label)
