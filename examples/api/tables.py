"""Markdown tables from bench.py's results.json, medians of the reps.

    python examples/api/tables.py [build/api-bench/results.json]
"""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "build", "api-bench", "results.json")
rep = json.load(open(path))
NAMES = {"gust": "Gust", "go": "Go net/http", "goraw": "Go, same HTTP as Gust"}

def f(v, fmt="{:,.0f}"):
    return "–" if v is None else fmt.format(v)

print("| scenario | server | req/s | p50 µs | p99 µs | p99.9 µs | peak RSS MB | server CPU | load gen CPU | errors |")
print("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|")
for sc in rep["scenarios"]:
    for kind, m in sc["median"].items():
        rate = "rate" in sc["name"]
        p = ("due_p50_us", "due_p99_us", "due_p999_us") if rate else ("p50_us", "p99_us", "p999_us")
        print(f"| {sc['name']} | {NAMES[kind]} | {f(m['rps'])} | {f(m.get(p[0]))} | {f(m.get(p[1]))} | "
              f"{f(m.get(p[2]))} | {f(m['peak_rss_mb'], '{:.1f}')} | {f(m['server_cpu_share'], '{:.0%}')} | "
              f"{f(m['client_cpu_share'], '{:.0%}')} | {m['errors'] + m['status_mismatches']} |")

print()
print("| scenario | server | page faults per 1k requests | heap allocations per request | heap bytes per request | collections | collector pause, total ms |")
print("|---|---|---:|---:|---:|---:|---:|")
for sc in rep["scenarios"]:
    for kind, m in sc["median"].items():
        print(f"| {sc['name']} | {NAMES[kind]} | {f(m.get('page_faults_per_1k_requests'), '{:.2f}')} | "
              f"{f(m.get('go_mallocs_per_request'), '{:.1f}') if kind != 'gust' else '0 (see text)'} | "
              f"{f(m.get('go_bytes_per_request'), '{:,.0f}') if kind != 'gust' else '0'} | "
              f"{f(m.get('go_gcs')) if kind != 'gust' else 'none'} | "
              f"{f(m.get('go_gc_pause_ms'), '{:.1f}') if kind != 'gust' else 'none'} |")

print()
print("| server | RSS per idle connection | commit per idle connection | threads for 500 connections |")
print("|---|---:|---:|---:|")
for kind, m in rep["idle"].items():
    print(f"| {NAMES[kind]} | {m['rss_kb_per_connection']:.0f} KB | {m['private_kb_per_connection']:.0f} KB | {m['threads']} |")
