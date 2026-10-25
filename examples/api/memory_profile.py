"""What an idle keep-alive connection costs each server, in memory and
threads, at several connection counts (Windows).

    python examples/api/memory_profile.py [--counts 0,100,250,500,1000] [--out file.json]

Every connection makes one request and then stays open, idle, inside both
servers' 5 s idle timeout. At each count the server's address space is read
with VirtualQueryEx, so the three kinds of memory are told apart:

- reserved: address space of the process's private regions, committed or
  not (Gust reserves 2 GB per data stack up front; the system gives it no
  memory until it is committed);
- committed: private memory the system has promised, which counts against
  its commit limit (RAM plus page file) whether or not it was touched;
- resident: the working set, pages actually in RAM (private and shared).

Each is also given per connection, as (value at N - value at 0) / N.
Uses the servers bench.py builds into build/api-bench/.
"""
import argparse, ctypes, json, os, signal, socket, subprocess, sys, time
from ctypes import wintypes
import psutil

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bench  # noqa: E402  (its Server class and build)

MEM_COMMIT, MEM_RESERVE, MEM_PRIVATE = 0x1000, 0x2000, 0x20000

class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wintypes.DWORD), ("PartitionId", wintypes.WORD),
                ("RegionSize", ctypes.c_size_t), ("State", wintypes.DWORD),
                ("Protect", wintypes.DWORD), ("Type", wintypes.DWORD)]

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.VirtualQueryEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
kernel32.VirtualQueryEx.restype = ctypes.c_size_t

PAGE_GUARD = 0x100

def address_space(pid):
    """Reserved and committed bytes of the process's private regions, in all
    and by kind: Gust's data stack regions (reservations of 1 GB or more),
    native thread stacks (reservations holding a guard page), and the rest
    (heaps, runtime structures, Go's arenas)."""
    h = kernel32.OpenProcess(0x0400 | 0x0010, False, pid)   # QUERY_INFORMATION | VM_READ
    allocs = {}     # allocation base -> [reserved, committed, has guard page]
    addr, mbi = 0, MBI()
    try:
        while addr < (1 << 47):
            if not kernel32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
                break
            if mbi.Type == MEM_PRIVATE and mbi.State in (MEM_COMMIT, MEM_RESERVE):
                a = allocs.setdefault(mbi.AllocationBase or 0, [0, 0, False])
                a[0] += mbi.RegionSize
                if mbi.State == MEM_COMMIT:
                    a[1] += mbi.RegionSize
                    if mbi.Protect & PAGE_GUARD:
                        a[2] = True
            addr = (mbi.BaseAddress or 0) + mbi.RegionSize
    finally:
        kernel32.CloseHandle(h)
    out = {"reserved": 0, "committed": 0, "data_stack_committed": 0, "thread_stack_committed": 0,
           "other_committed": 0}
    for reserved, committed, guard in allocs.values():
        out["reserved"] += reserved
        out["committed"] += committed
        kind = "data_stack" if reserved >= (1 << 30) else "thread_stack" if guard else "other"
        out[f"{kind}_committed"] += committed
    return out

def sample(srv):
    a = address_space(srv.proc.pid)
    return {**{f"{k}_mb": v / 2**20 for k, v in a.items()},
            "resident_mb": srv.ps.memory_info().rss / 2**20, "threads": srv.ps.num_threads()}

def profile(kind, counts, extra=()):
    srv = bench.Server(kind, extra)
    socks, rows = [], []
    try:
        time.sleep(0.3)
        for n in counts:
            while len(socks) < n:
                # Blocking, no timeout (see bench.idle_memory).
                s = socket.create_connection(("127.0.0.1", srv.port))
                s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
                s.recv(4096)
                socks.append(s)
            time.sleep(0.3)
            row = {"connections": n, **sample(srv)}
            rows.append(row)
        base = rows[0]
        for row in rows[1:]:
            n = row["connections"]
            for k in ("reserved", "committed", "resident", "data_stack_committed", "thread_stack_committed",
                      "other_committed"):
                row[f"{k}_kb_per_connection"] = (row[f"{k}_mb"] - base[f"{k}_mb"]) * 1024 / n
            row["threads_per_connection"] = (row["threads"] - base["threads"]) / n
    finally:
        for s in socks:
            s.close()
        srv.stop()
    return rows

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--counts", default="0,100,250,500,1000")
    ap.add_argument("--kinds", default=",".join(bench.KINDS))
    ap.add_argument("--out", default=os.path.join(bench.OUT, "memory_profile.json"))
    ap.add_argument("--no-build", action="store_true")
    args = ap.parse_args()
    if not args.no_build:
        bench.build()
    counts = [int(c) for c in args.counts.split(",")]
    report = {}
    for kind in args.kinds.split(","):
        report[kind] = profile(kind, counts)
        for row in report[kind]:
            per = (f"  per connection: reserved {row['reserved_kb_per_connection']:9.0f} KB, committed "
                   f"{row['committed_kb_per_connection']:6.1f} KB (data stacks "
                   f"{row['data_stack_committed_kb_per_connection']:.1f}, thread stacks "
                   f"{row['thread_stack_committed_kb_per_connection']:.1f}, other {row['other_committed_kb_per_connection']:.1f}), "
                   f"resident {row['resident_kb_per_connection']:6.1f} KB, "
                   f"threads {row['threads_per_connection']:.2f}") if row["connections"] else ""
            print(f"{kind:9s} {row['connections']:5d} connections: reserved {row['reserved_mb']:10.1f} MB, committed "
                  f"{row['committed_mb']:7.1f} MB, resident {row['resident_mb']:6.1f} MB, threads {row['threads']:5d}{per}",
                  flush=True)
    with open(args.out, "w") as f:
        json.dump(report, f, indent=1)
    print("wrote", args.out)

if __name__ == "__main__":
    main()
