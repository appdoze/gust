"""Markdown tables from bench.py's results.json: medians of the reps, with
the range (lowest to highest rep) for throughput and the tail.

    python examples/api/tables.py [build/api-bench/results.json]
"""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "build", "api-bench", "results.json")
rep = json.load(open(path))
NAMES = {"gust": "Gust, worker pool", "gustthreads": "Gust, thread per connection", "go": "Go net/http",
         "goraw": "Go, same HTTP as Gust", "gustbefore": "Gust, worker pool, before (bench.py --before)"}

def f(v, fmt="{:,.0f}"):
    return "–" if v is None else fmt.format(v)

def spread(m, k, fmt="{:,.0f}"):
    lo, hi = m.get(k + "_min"), m.get(k + "_max")
    return f"{f(m.get(k), fmt)} ({f(lo, fmt)}–{f(hi, fmt)})" if lo is not None else f(m.get(k), fmt)

def latency_table(title, kinds):
    print(f"**{title}**")
    print()
    print("| scenario | server | req/s (range) | p50 µs | p95 µs | p99 µs (range) | p99.9 µs (range) | peak RSS MB | server CPU | load gen CPU | errors |")
    print("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for sc in rep["scenarios"]:
        if "rate" in sc["name"]:
            continue
        for kind, m in sc["median"].items():
            if kind not in kinds:
                continue
            print(f"| {sc['name']} | {NAMES.get(kind, kind)} | {spread(m, 'rps')} | {f(m.get('p50_us'))} | "
                  f"{f(m.get('p95_us'))} | {spread(m, 'p99_us')} | {spread(m, 'p999_us')} | "
                  f"{f(m['peak_rss_mb'], '{:.1f}')} | {f(m['server_cpu_share'], '{:.0%}')} | "
                  f"{f(m['client_cpu_share'], '{:.0%}')} | {m['errors'] + m['status_mismatches']} |")
    print()
    rates = [sc for sc in rep["scenarios"] if "rate" in sc["name"]]
    if not rates:
        return
    print("At a fixed rate: service time (from the request going out) and, in the last three columns, the")
    print("time from when it was due (coordinated omission corrected; includes the load generator's timer slack).")
    print()
    print("| scenario | server | p50 µs | p99 µs (range) | p99.9 µs (range) | due p50 µs | due p99 µs (range) | due p99.9 µs (range) |")
    print("|---|---|---:|---:|---:|---:|---:|---:|")
    for sc in rates:
        for kind, m in sc["median"].items():
            if kind in kinds:
                print(f"| {sc['name']} | {NAMES.get(kind, kind)} | {f(m.get('p50_us'))} | {spread(m, 'p99_us')} | "
                      f"{spread(m, 'p999_us')} | {f(m.get('due_p50_us'))} | {spread(m, 'due_p99_us')} | "
                      f"{spread(m, 'due_p999_us')} |")
    print()

def before_after_table():
    """The pool before and after a change (bench.py --before), with the
    machine's CPU use just before each run, which neither controls."""
    print("**Before and after**")
    print()
    print("| scenario | server | req/s (range) | p50 µs | p99 µs (range) | p99.9 µs (range) | peak RSS MB | server CPU | other CPU before | errors |")
    print("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for sc in rep["scenarios"]:
        for kind in ("gustbefore", "gust"):
            m = sc["median"].get(kind)
            if m is None:
                continue
            print(f"| {sc['name']} | {NAMES[kind]} | {spread(m, 'rps')} | {f(m.get('p50_us'))} | "
                  f"{spread(m, 'p99_us')} | {spread(m, 'p999_us')} | {f(m['peak_rss_mb'], '{:.1f}')} | "
                  f"{f(m['server_cpu_share'], '{:.0%}')} | {f(m.get('background_cpu_pct'), '{:.0f}%')} | "
                  f"{m['errors'] + m['status_mismatches']} |")
    print()

if any("gustbefore" in sc["median"] for sc in rep["scenarios"]):
    before_after_table()
else:
    # The two comparisons apart: the same minimal HTTP in each language, then
    # Gust's server against Go's standard library server.
    latency_table("Equivalent minimal HTTP", ("gust", "gustthreads", "goraw"))
    latency_table("Gust against Go's net/http", ("gust", "go"))

print()
print("| scenario | server | page faults per 1k requests | heap allocations per request | heap bytes per request | collections | collector pause, total ms |")
print("|---|---|---:|---:|---:|---:|---:|")
for sc in rep["scenarios"]:
    for kind, m in sc["median"].items():
        gust = kind.startswith("gust")
        print(f"| {sc['name']} | {NAMES.get(kind, kind)} | {f(m.get('page_faults_per_1k_requests'), '{:.2f}')} | "
              f"{'0 (see text)' if gust else f(m.get('go_mallocs_per_request'), '{:.1f}')} | "
              f"{'0' if gust else f(m.get('go_bytes_per_request'), '{:,.0f}')} | "
              f"{'none' if gust else f(m.get('go_gcs'))} | "
              f"{'none' if gust else f(m.get('go_gc_pause_ms'), '{:.1f}')} |")

print()
print("| server | resident per idle connection | committed per idle connection | threads for 500 connections |")
print("|---|---:|---:|---:|")
for kind, m in rep["idle"].items():
    print(f"| {NAMES.get(kind, kind)} | {m['rss_kb_per_connection']:.0f} KB | {m['private_kb_per_connection']:.0f} KB | {m['threads']} |")
