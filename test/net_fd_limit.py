"""Gust: a server out of file descriptors (Linux and macOS). The echo example
runs with its descriptor limit set low, and more clients connect than it can
take. The connections it cannot take are dropped at once (the runtime's
reserve descriptor), the listener rests rather than wake the worker for
nothing, so the server does not spin, and once the clients go it serves
again. Built as api_example.py builds the examples."""
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import toolchain as tc
from api_example import build_example

EXAMPLE = Path(__file__).resolve().parent.parent / "examples" / "echo" / "echo.gust"
LIMIT = 40          # descriptors: the standard three, the runtime's, a few dozen sockets
CLIENTS = 120


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def cpu_seconds(pid):
    """The process's user and system time from /proc (Linux); None elsewhere."""
    try:
        fields = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
        return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")
    except OSError:
        return None


def check_net_fd_limit(exe, base, cc=None, extra=(), runtime=None):
    """Returns why it was skipped, or None when every check passed; raises
    RuntimeError on a failure."""
    if tc.IS_WINDOWS:
        return "not Linux or macOS"
    import resource
    base = Path(base).resolve()
    base.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fdlimit-", dir=base) as tmp:
        root = Path(tmp)
        env = dict(os.environ, GOOSE_HOME=str(root / "home"))
        app = root / "echo"
        build_example(exe, EXAMPLE, app, [], env, cc, extra, runtime)
        port = free_port()

        def low_limit():
            resource.setrlimit(resource.RLIMIT_NOFILE, (LIMIT, LIMIT))

        proc = subprocess.Popen([str(app), str(port), "--workers", "1"], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, preexec_fn=low_limit)
        clients = []
        try:
            deadline = time.time() + 10
            while time.time() < deadline:
                try:
                    socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                    break
                except OSError:
                    time.sleep(0.05)
            for _ in range(CLIENTS):
                try:
                    clients.append(socket.create_connection(("127.0.0.1", port), timeout=2))
                except OSError:
                    pass
            for s in clients:
                try:
                    s.sendall(b"x")
                except OSError:
                    pass
            time.sleep(1.0)
            # The ones it took echo; the ones it could not were closed at once
            # (an end of file or a reset), not left waiting.
            served = dropped = waiting = 0
            for s in clients:
                s.settimeout(0.05)
                try:
                    got = s.recv(1)
                    if got == b"x":
                        served += 1
                    else:
                        dropped += 1
                except socket.timeout:
                    waiting += 1
                except OSError:
                    dropped += 1
            if served == 0 or dropped == 0:
                raise RuntimeError(f"fd limit: of {len(clients)} clients {served} were served, {dropped} "
                                   f"dropped and {waiting} left waiting; some should be served and some "
                                   "dropped")
            # Not spinning: while the clients it took sit idle, and more wait,
            # the worker uses little CPU.
            before = cpu_seconds(proc.pid)
            extra_clients = [socket.create_connection(("127.0.0.1", port), timeout=2) for _ in range(20)]
            time.sleep(1.0)
            after = cpu_seconds(proc.pid)
            for s in extra_clients:
                s.close()
            if before is not None and after - before > 0.5:
                raise RuntimeError(f"fd limit: the server used {after - before:.2f} s of CPU in 1 s "
                                   "while out of descriptors")
            for s in clients:
                s.close()
            clients = []
            time.sleep(0.5)
            with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
                s.settimeout(5)
                s.sendall(b"again")
                if s.recv(5) != b"again":
                    raise RuntimeError("fd limit: no echo once the clients had gone")
        finally:
            for s in clients:
                s.close()
            proc.send_signal(2)
            try:
                out, err = proc.communicate(timeout=20)
            except subprocess.TimeoutExpired:
                proc.kill()
                out, err = proc.communicate()
        text = out.decode(errors="replace")
        if proc.returncode != 0 or "accept(s) failed" not in text or " 0 accept(s) failed" in text:
            raise RuntimeError(f"fd limit: exit {proc.returncode}, the server said: {text.strip()} "
                               f"{err.decode(errors='replace').strip()}")
    return None


if __name__ == "__main__":
    print(check_net_fd_limit(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "build/gen/manual") or "ok")
