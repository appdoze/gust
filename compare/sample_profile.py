#!/usr/bin/env python3
"""Gust: where a program's time goes, on Windows, with nothing installed:
the main thread is stopped about once a millisecond, its instruction
pointer read (GetThreadContext) and the thread resumed; each sample counts
for the function the linker's map file says holds that address.

    python compare/sample_profile.py --map prog.map [--top 25] -- prog.exe args...

The map is the one MSVC's link.exe or lld-link writes with /MAP (cl ...
/link /MAP:prog.map; clang -Wl,/MAP:prog.map). The process runs pinned to
one core, as compare/quiet_bench.py's do. Samples outside the executable
(the C library, the kernel) count under the module they fall in.
"""
import argparse, bisect, collections, ctypes, os, re, subprocess, sys, time
from ctypes import wintypes

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenThread.restype = wintypes.HANDLE
k32.OpenThread.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
k32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
k32.GetCurrentProcess.restype = wintypes.HANDLE
k32.SetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
k32.SuspendThread.restype = wintypes.DWORD
k32.SuspendThread.argtypes = [wintypes.HANDLE]
k32.ResumeThread.restype = wintypes.DWORD
k32.ResumeThread.argtypes = [wintypes.HANDLE]
k32.GetThreadContext.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
k32.CloseHandle.argtypes = [wintypes.HANDLE]
for f in ("Thread32First", "Thread32Next", "Module32FirstW", "Module32NextW"):
    getattr(k32, f).argtypes = [wintypes.HANDLE, ctypes.c_void_p]

TH32CS_SNAPTHREAD, TH32CS_SNAPMODULE, TH32CS_SNAPMODULE32 = 0x4, 0x8, 0x10
THREAD_ACCESS = 0x0002 | 0x0008 | 0x0040   # suspend/resume, get context, query
CONTEXT_CONTROL = 0x00100001
CONTEXT_SIZE, RIP_OFFSET, FLAGS_OFFSET = 1232, 0xF8, 0x30


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ThreadID", wintypes.DWORD),
                ("th32OwnerProcessID", wintypes.DWORD), ("tpBasePri", wintypes.LONG),
                ("tpDeltaPri", wintypes.LONG), ("dwFlags", wintypes.DWORD)]


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("th32ModuleID", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                ("GlblcntUsage", wintypes.DWORD), ("ProccntUsage", wintypes.DWORD),
                ("modBaseAddr", ctypes.c_void_p), ("modBaseSize", wintypes.DWORD), ("hModule", wintypes.HMODULE),
                ("szModule", ctypes.c_wchar * 256), ("szExePath", ctypes.c_wchar * 260)]


def first_thread(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    tid = None
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            tid = te.th32ThreadID
            break
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    return tid


def modules(pid):
    """[(base, end, name)] of the process's modules."""
    for _ in range(50):
        snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
        if snap and snap != wintypes.HANDLE(-1).value:
            break
        time.sleep(0.002)
    me = MODULEENTRY32W()
    me.dwSize = ctypes.sizeof(me)
    out = []
    ok = k32.Module32FirstW(snap, ctypes.byref(me))
    while ok:
        out.append((me.modBaseAddr, me.modBaseAddr + me.modBaseSize, me.szModule))
        ok = k32.Module32NextW(snap, ctypes.byref(me))
    k32.CloseHandle(snap)
    return out


def read_map(path):
    """The preferred load address and [(address, name)] sorted, from a /MAP file."""
    base = 0x140000000
    syms = []
    for line in open(path, errors="replace"):
        m = re.search(r"Preferred load address is ([0-9a-fA-F]+)", line)
        if m:
            base = int(m.group(1), 16)
            continue
        f = line.split()
        if len(f) >= 3 and re.fullmatch(r"[0-9a-fA-F]{4}:[0-9a-fA-F]{8}", f[0]) and re.fullmatch(r"[0-9a-fA-F]{16}", f[2]):
            addr = int(f[2], 16)
            if addr:
                syms.append((addr, f[1]))
    syms.sort()
    return base, syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", required=True)
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--core", type=int, default=6)
    ap.add_argument("--interval", type=float, default=0.001)
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.cmd[1:] if a.cmd and a.cmd[0] == "--" else a.cmd
    k32.SetProcessAffinityMask(k32.GetCurrentProcess(), ctypes.c_size_t(1 << a.core))
    base, syms = read_map(a.map)
    addrs = [s[0] for s in syms]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL)
    tid = None
    while tid is None and p.poll() is None:
        tid = first_thread(p.pid)
    mods = modules(p.pid)
    exe = os.path.basename(cmd[0]).lower()
    load = next((b for b, e, n in mods if n.lower() == exe), None)
    h = k32.OpenThread(THREAD_ACCESS, False, tid)
    buf = ctypes.create_string_buffer(CONTEXT_SIZE + 16)
    ctx = (ctypes.addressof(buf) + 15) & ~15
    counts = collections.Counter()
    total = 0
    while p.poll() is None:
        if k32.SuspendThread(h) == 0xFFFFFFFF:
            break
        ctypes.c_uint32.from_address(ctx + FLAGS_OFFSET).value = CONTEXT_CONTROL
        got = k32.GetThreadContext(h, ctypes.c_void_p(ctx))
        rip = ctypes.c_uint64.from_address(ctx + RIP_OFFSET).value if got else 0
        k32.ResumeThread(h)
        if got:
            total += 1
            if load is not None and load <= rip < load + (1 << 31):
                i = bisect.bisect_right(addrs, rip - load + base) - 1
                counts[syms[i][1] if i >= 0 else "?"] += 1
            else:
                counts[next((f"[{n}]" for b, e, n in mods if b <= rip < e), "[other]")] += 1
        time.sleep(a.interval)
    k32.CloseHandle(h)
    print(f"{total} samples of {' '.join(cmd)}")
    for name, n in counts.most_common(a.top):
        print(f"  {100.0 * n / max(total, 1):5.1f}%  {name}")


if __name__ == "__main__":
    main()
