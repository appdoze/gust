"""Gust: C that has the kernel write into memory Gust passed it. On Windows
and macOS a data stack is committed by a fault handler as user-mode code
touches it, and a system call writing into a page never touched fails
rather than faulting it in (runtime_commit_chunks.c checks that rule). Gust
only ever passes C memory it has initialized, so the pages are committed: a
growable array resized past many commit chunks, a struct's large inline
array, a limited array's elements, and the same in a fresh worker, whose
stacks start empty. Built with `gust build`; the C reads a file with the
OS call itself (ReadFile, read)."""
import os
import subprocess
import tempfile
from pathlib import Path

import toolchain as tc

SYS_READ_C = r'''
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
int64_t sys_read(const uint8_t *path, uint8_t *buf, int64_t n) {
    HANDLE h = CreateFileA((const char *)path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)n, &got, NULL);
    CloseHandle(h);
    return ok ? (int64_t)got : -1000 - (int64_t)GetLastError();
}
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
int64_t sys_read(const uint8_t *path, uint8_t *buf, int64_t n) {
    int fd = open((const char *)path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t got = read(fd, buf, (size_t)n);
    close(fd);
    return got >= 0 ? (int64_t)got : -1000 - (int64_t)errno;
}
#endif
'''

PROGRAM = '''import std;
extern fn sys_read(path: const u8&, buf: u8&, n: i64) -> i64;

struct Big { n: i64, bytes: u8[300000] }

// Reads into each kind of Gust memory and reports what arrived.
fn probe(who: const u8[:]) {
    let path = "input.bin\\0";
    var a: u8[>..] = [];
    a.resize(5000000, 0);
    let r1 = sys_read(path[0], a[4900000], 100000);
    var b = Big { n: 1, .. };
    let r2 = sys_read(path[0], b.bytes[0], 300000);
    var c: u8[..70000] = [];
    c.resize(70000, 0);
    let r3 = sys_read(path[0], c[0], 70000);
    print(who, ": ", r1, " ", a[4900000], " ", a[4999999], "; ", r2, " ", b.bytes[299999], "; ",
          r3, " ", c[69999]);
}

thread_fn worker() { probe("worker"); }

fn main() {
    probe("main");
    thread_wait(thread_spawn(worker));
}
'''


def check_ffi_stack_writes(exe, base):
    """Returns why it was skipped, or None when it ran; raises RuntimeError
    on a failure."""
    base = Path(base).resolve()
    base.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ffiwrites-", dir=base) as tmp:
        root = Path(tmp)
        env = dict(os.environ)
        env["GOOSE_HOME"] = str(root / "home")
        tc.write_text(root / "probe.gust", PROGRAM)
        tc.write_text(root / "sys_read.c", SYS_READ_C)
        tc.write_text(root / "probe.build", 'c_sources = ["sys_read.c"];\n')
        (root / "input.bin").write_bytes(bytes(i % 251 for i in range(300000)))
        app = root / ("probe" + tc.EXE_SUFFIX)
        r = subprocess.run([str(exe), "build", "-o", str(app), str(root / "probe.gust")], cwd=root, env=env,
                           capture_output=True, text=True, errors="replace", timeout=600)
        if r.returncode != 0:
            raise RuntimeError(f"ffi stack writes: gust build failed:\n{r.stdout}{r.stderr}")
        r = subprocess.run([str(app)], cwd=root, capture_output=True, text=True, errors="replace", timeout=120)
        # Byte k of the file is k % 251: 99999 % 251 = 101, 299999 % 251 = 54, 69999 % 251 = 221.
        want = [f"{who}: 100000 0 101; 300000 54; 70000 221" for who in ("main", "worker")]
        if r.returncode != 0 or r.stdout.split("\n")[:2] != want:
            raise RuntimeError(f"ffi stack writes: wanted {want}, got exit {r.returncode}:\n{r.stdout}{r.stderr}")
    return None
