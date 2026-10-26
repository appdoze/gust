# compare/: Gust against Rust and Lobster

The cross-language suite the roadmap measures every phase against: eight
benchmarks implemented once per language, and safety probes, each a
program with one classic bug.

| path | what |
|---|---|
| `SPEC.md` | benchmarks 1-5 (nbody, raytrace, interp, sort, astar), from Lobster's `tests/compare/` |
| `EXTRA_SPEC.md` | benchmarks 6-8 (bintrees, hashmap, strings), which that suite leaves out |
| `gust/main.goose`, `rust/main.rs`, `lobster/*.lobster` | one program per language, running all eight in-process and timing each |
| `run_bench.py` | builds every configuration this machine has, runs them round-robin, checks the checksums agree, measures peak memory; `results.json` is its output |
| `safety/` | the probes per language, `run_safety.py`, `expected.json` (what each Gust probe must do), `results.json` |
| `make_tables.py` | Markdown tables from both results files |

```bash
python compare/run_bench.py
```

```bash
python compare/safety/run_safety.py
```

```bash
python compare/make_tables.py
```

**CI** runs both with `--check` on Windows, Linux and macOS: every Gust
build of the benchmarks (clang, `--checked`, the TinyCC JIT, MSVC on
Windows, gcc on Linux) and Rust's must agree on every checksum, and every
Gust probe in release, `--checked` and `--debug` must do what
`safety/expected.json` says. Timings from CI mean nothing; `results.json`
holds a full run on one machine, the baseline later phases are measured
against, and says what that machine was.

**Rules** (SPEC.md): the same algorithm and order in every language,
idiomatic data structures, in-process timing (the best of `repeats`), no
output in the timed region, and one `bench<TAB>name<TAB>seconds<TAB>checksum`
line per benchmark. A faster result from a different algorithm does not
count.

**Tools.** `GUST` names the compiler (default: `build/Release/gust`);
Rust runs where `rustc` is on the path; Lobster where `LOBSTER` names
`lobster.exe` or its repository root, and its C++ builds on Windows also
need `LOBSTER_MSBUILD`. A missing tool skips its configurations.

**Provenance and licences.** `SPEC.md`, `lobster/` (but for
`bintrees.lobster`, `hashmap.lobster`, `strings.lobster` and the eight-
benchmark `main.lobster`) and benchmarks 1-5 of `rust/main.rs` come from
Lobster's `tests/compare/` (https://github.com/aardappel/lobster, Wouter van
Oortmerssen), Apache-2.0, unchanged. The rest (`EXTRA_SPEC.md`, the Gust
program, benchmarks 6-8 in Rust and Lobster, the probes and the scripts)
was written for this comparison, under Gust's licence (Apache-2.0).
