"""Markdown tables from results.json (run_bench.py) and safety/results.json
(safety/run_safety.py).

    python compare/make_tables.py > compare/tables.md
"""

import json
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
BENCHES = ["nbody", "raytrace", "interp", "sort", "astar", "bintrees", "hashmap", "strings"]


def geomean(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


def main():
    r = json.load(open(os.path.join(HERE, "results.json")))
    ok = [c for c in r["configs"] if c["status"] == "ok" and len(c["seconds"]) == len(BENCHES)]
    best = {b: min(c["seconds"][b] for c in ok) for b in BENCHES}

    print(f"Machine: {r['machine']}. Generated {r['generated']}; each configuration launched "
          f"{r['runs']} times, best of {r['repeats']} in-process repeats.\n")

    print("### Seconds per benchmark (lower is better)\n")
    print("| configuration | " + " | ".join(BENCHES) + " | geomean vs best |")
    print("|---|" + "---:|" * (len(BENCHES) + 1))
    rows = []
    for c in ok:
        g = geomean([c["seconds"][b] / best[b] for b in BENCHES])
        rows.append((g, c))
    for g, c in sorted(rows, key=lambda t: t[0]):
        cells = []
        for b in BENCHES:
            s = c["seconds"][b]
            cells.append(f"**{s:.3f}**" if s == best[b] else f"{s:.3f}")
        print(f"| {c['name']} | " + " | ".join(cells) + f" | {g:.2f}x |")

    print("\n### Slowdown relative to the fastest configuration per benchmark\n")
    print("| configuration | " + " | ".join(BENCHES) + " |")
    print("|---|" + "---:|" * len(BENCHES))
    for g, c in sorted(rows, key=lambda t: t[0]):
        print(f"| {c['name']} | " + " | ".join(f"{c['seconds'][b] / best[b]:.2f}x" for b in BENCHES) + " |")

    print("\n### Peak working set per benchmark, MB (one benchmark per process, lower is better)\n")
    print("| configuration | idle | " + " | ".join(BENCHES) + " |")
    print("|---|" + "---:|" * (len(BENCHES) + 1))
    for g, c in sorted(rows, key=lambda t: t[0]):
        mem = c.get("peak_bytes", {})
        cells = [f"{mem[b] / 1e6:.1f}" if mem.get(b) else "-" for b in BENCHES]
        idle = c.get("startup_peak_bytes")
        print(f"| {c['name']} | {idle / 1e6:.1f} | " + " | ".join(cells) + " |")

    print("\n### Build and startup\n")
    print("| configuration | build (s) | binary (MB) | launch with no work (s) | how it runs |")
    print("|---|---:|---:|---:|---|")
    for c in r["configs"]:
        b = f"{c['build_seconds']:.1f}" if "build_seconds" in c else "-"
        size = f"{c['binary_bytes'] / 1e6:.2f}" if "binary_bytes" in c else "-"
        st = f"{c['startup_seconds']:.2f}" if "startup_seconds" in c else "-"
        print(f"| {c['name']} | {b} | {size} | {st} | {c['note']} |")

    print("\n### Checksums\n")
    print("| benchmark | value | all configurations agree |")
    print("|---|---:|---|")
    ref = ok[0]["checksums"]
    for b in BENCHES:
        agree = not any(p.startswith(b + ":") for p in r["checksum_problems"])
        print(f"| {b} | {ref[b]!r} | {'yes' if agree else 'NO'} |")

    print("\n### Source size: non-blank, non-comment lines\n")
    src = r["source"]
    names = ["shared"] + BENCHES + ["driver"]
    print("| part | Rust | Gust | Lobster |")
    print("|---|---:|---:|---:|")
    totals = {"rust": 0, "gust": 0, "lobster": 0}
    for n in names:
        cells = []
        for lang in ["rust", "gust", "lobster"]:
            v = src[lang].get(n, {}).get("lines")
            totals[lang] += v or 0
            cells.append(str(v) if v is not None else "-")
        print(f"| {n} | " + " | ".join(cells) + " |")
    print(f"| **total** | **{totals['rust']}** | **{totals['gust']}** | **{totals['lobster']}** |")

    path = os.path.join(HERE, "safety", "results.json")
    if os.path.exists(path):
        safety = json.load(open(path))
        modes = sorted({m for probe in safety.values() for m in probe})
        print("\n### Safety probes: what each build did\n")
        print("| probe | " + " | ".join(modes) + " |")
        print("|---|" + "---|" * len(modes))
        for name, probe in sorted(safety.items()):
            cells = []
            for m in modes:
                v = probe.get(m)
                if not v:
                    cells.append("-")
                elif v["stage"] == "compile":
                    cells.append("compile error")
                elif v["exit"] == "timeout":
                    cells.append("still running (timeout)")
                elif v["exit"] == 0:
                    cells.append("ran: " + v.get("stdout", v.get("output", ""))[:40].replace("|", "/"))
                else:
                    cells.append(f"stopped (exit {v['exit']})")
            print(f"| {name} | " + " | ".join(cells) + " |")


if __name__ == "__main__":
    main()
