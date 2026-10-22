# Gust roadmap

From fork to a 1.0 release that is faster than Rust across `compare/`, at least as safe, and
usable with large libraries and multiple cores. The durations assume **one experienced
developer working full time with AI coding agents**; a small team can run some phases in
parallel (3 with 2, 6 with 5). They are estimates, not measurements.

"The fork" below is Gust. Every phase ends with exit criteria
checked by three suites: Goose's own tests (`python test/run_tests.py`, 3,551 files), the
cross-language benchmark harness (`python compare/run_bench.py`: checksums must stay identical
and nothing may get slower) and the safety probes (`python compare/safety/run_safety.py`).
Both move into `compare/` in Phase 0.

## Overview

| # | Phase | When | Result |
|---|---|---|---|
| 0 | Setup | week 1 | Renamed fork, full history, CI running all three suites |
| 1 | Quick wins | weeks 2–4 | clang by default, checked release builds, `simd fn` fixed |
| 2 | Code quality | months 2–3 | Ahead of Rust on all 8 benchmarks, single-threaded |
| 3 | Library access | months 3–5 | C callbacks, automatic C header import, LGPL-free JIT path |
| 4 | Multicore | months 5–8 | Safe `parallel for`, shared frozen data, zero-copy trees |
| 5 | Vectorization | months 8–10 | Automatic lane vectorization (nbody 2x Rust, no source changes) |
| 6 | Developer experience | months 10–13 | MIR JIT, Python bindings, formatter, package registry |
| 7 | Expressiveness | months 12–16 | Moves, labeled break, compile-time plugins, async I/O, HTTP |
| 8 | Hardening and 1.0 | months 16–24 | Fuzzing, soundness review, frozen spec, 1.0 release |
| – | After 1.0 | years | Ecosystem growth: libraries, users, contributors |

**Roughly two years to 1.0**, then ongoing ecosystem work. A language is never "finished";
1.0 is the point where you promise not to break people's code.

## Phase 0: Setup (week 1)

- Choose the name: **Gust** (done).
- Fork with **full history** and keep `aardappel/goose` as the `upstream` remote. **(done)**
- License housekeeping: keep `LICENSE` and every copyright notice, add "modified by"
  notices to changed files, credit Goose in the README, rename the binary. **(done; the docs
  keep Goose's name until the hard fork)**
- Move the cross-language benchmarks and safety probes into `compare/` (Lobster's suite is
  Apache-2.0 too).
- Commit the clang-discovery fix as the first change. **(done)**
- CI on Windows, Linux and macOS running Goose's tests plus `compare/` and `compare/safety/`.
- Record today's numbers (`compare/results.json`) as the baseline every later phase is
  measured against.

**Exit:** CI green on three platforms; baseline stored.

## Phase 1: Quick wins (weeks 2–4)

- **clang as the default C compiler** wherever it is installed. **(done on Windows,
  including CMake packages under clang; confirm on Linux and macOS)**
- **`--checked`**: an optimized build that keeps the overflow, range and tag checks
  `--debug` has. **(done)** Measured cost on the 8 benchmarks: 0-1% on four, 6-7% on sort
  and astar, 12% on hashmap, 17% on bintrees, about 5% on average; still level with or
  ahead of unchecked Rust on all eight. Stays opt-in until Phase 2's range analysis
  removes more of the checks.
- **`extern fn` audit**: `--externs` lists every `extern fn` outside the standard library,
  `--deny-externs` refuses them, the equivalent of auditing Rust's `unsafe`. **(done)**
- **`simd fn`**: its AVX2 / AVX-512 copies can be *slower* than the plain function
  (measured: 0.160 s against 0.105 s on the nbody lane prototype). Investigated: the cause
  is not the per-function `target` attribute; clang's SLP vectorizer does not vectorize
  the loop in a standalone copy at all, with or without the attribute, at any threshold,
  and only did in the plain version after inlining it into its caller. Writing the lanes
  as explicit vector types fixed it (0.077 s), so the fix belongs with Phase 5's lane
  pass. Until then, put `simd` on the function the hot loop is inlined into.
- **Generation checks for `reusable` pools** in debug builds, so a stale reference aborts
  instead of reading another value (safety probe 9).
- Send the general fixes (clang discovery, CMake packages under clang) upstream as pull
  requests while the fork is still close to Goose.

**Exit:** a test shows `--checked` catching overflow in an optimized build **(done)**; no
benchmark regresses; generation checks for pools land. The `simd fn` criterion moves to
Phase 5.

## Phase 2: Code quality (months 2–3)

- Emit **`restrict`** where the compiler has proved two references have different roots:
  Rust's no-alias advantage without its borrowing rules.
- Emit range hints (`__builtin_assume`) from index types (`u32 in a`).
- Close the bounds-check gaps in Goose's spec (Appendix B, 0a and 0f): indices loaded from
  arrays, disjunctive loop exits, `u64` ranges. 46 of 161 index checks remain in the `compare/` programs.
- Find out why Lobster (same author) runs `interp` 1.24x faster than Goose under clang.
- Bundle clang (or an LLVM toolchain) with releases, so users without it don't get MSVC's
  17–28% slower code.

**Exit:** goose/clang strictly faster than Rust LTO on all 8 benchmarks (nbody and raytrace
are ties today); `interp` at or below Lobster's 0.419 s.

## Phase 3: Library access (months 3–5)

- **C callbacks**: pass an `export fn` to C as a function pointer, with C's `void*`
  user-data mapped to a typed reference. No closure objects, so the language's rules hold.
- **Automatic C header import** (`import c "sqlite3.h"`) using TinyCC's parser, with raw
  imports confined to auditable boundary modules.
- Prove it on four real libraries: SQLite, libcurl, zlib, OpenSSL.
- **Licensing**: move TinyCC to a separate DLL (satisfies LGPL relinking) or plan its
  replacement by MIR in Phase 6.

**Exit:** a program using SQLite and libcurl with no hand-written `extern fn`; C libraries
that need callbacks (libcurl's write callback) work.

## Phase 4: Multicore (months 5–8)

Write the spec first; these change the language.

- **Safe `parallel for`**: workers read the enclosing scope without copying and each
  iteration writes only its own part of an output. The root analysis proves it race-free;
  no `Send`/`Sync`, no lifetimes.
- **Work-stealing runtime**, so loops balance themselves.
- **Deterministic reductions**: the same floating-point result at any thread count.
- **Freeze-then-share**: large read-only data built at run time, read by every worker in
  place.
- **Zero-copy tree transfer**: structures linked by relative references cross a queue as one
  block, or by handing over the memory region (Goose's open item 9).
- Add multithreaded versions of raytrace and nbody to `compare/`, with Rust + rayon
  equivalents, and new safety probes that try to race through `parallel for`.

**Exit:** parallel raytrace and nbody at least as fast as Rust + rayon; every race probe is a
compile error.

## Phase 5: Vectorization (months 8–10)

- **Automatic outer-loop lane vectorization**: find loops whose iterations are independent
  (nbody's bodies), check legality, and run 2/4/8 of them per vector instruction while
  keeping each lane's floating-point order.
- Emit the lanes as explicit C vector types (`vector_size`), not hoping for clang's
  auto-vectorizer, which failed in `simd fn` copies (measured).
- Tie into `simd fn` dispatch for AVX2 / AVX-512 at run time.

**Exit:** unmodified `compare/gust/main.goose` nbody at least 2x faster than Rust with an
identical checksum (the hand prototype reached 2.1x); `simd fn` copies never slower than
the plain function.

## Phase 6: Developer experience (months 10–13)

- **MIR JIT** for no-build runs (Lobster's MIR mode is 2.56x off the fastest build, against
  4.39x for Goose's TinyCC mode; MIR is MIT-licensed, ending the LGPL question).
- **Python bindings generator**: turn a library into a pip-installable module, the route
  Rust grew through with PyO3.
- Formatter, a package registry with version resolution and a lockfile, a docs site.
- Keep error output agent-friendly (`--diagnostics json` exists) and ship a compact
  language guide AI agents can load in one go.

**Exit:** a fork library installed with `pip` and called from Python; no-build runs within
1.5x of compiled speed.

## Phase 7: Expressiveness (months 12–16)

- From Goose's own open items: moves for resizable values, more than one resizable per
  struct, labeled `break`.
- **Compile-time plugin registration** with `const for` (routers, event handlers resolved at
  compile time), instead of closures-as-objects.
- **Thread-per-core async I/O**: each worker owns its connections and event loop; then an
  HTTP server and client library on it.

**Exit:** an HTTP server and a callback-heavy GUI program written idiomatically; HTTP
throughput compared against a Rust (tokio) server in `compare/`.

## Phase 8: Hardening and 1.0 (months 16–24)

- Fuzz the compiler, the type checker and `from_bytes`; sanitizer jobs in CI.
- A soundness review of the memory model and the new parallel features, ideally with
  outside reviewers. "Safer than Rust" has to be earned by people trying to break it.
- Freeze the spec; publish a stability and edition policy.
- Release 1.0 with a published benchmark and safety report (`compare/`, rerun).

**Exit:** no known soundness bugs; ahead of Rust on every benchmark in `compare/`; 1.0 tagged.

## Upstream policy

- **Phases 0–3: soft fork.** Merge Goose weekly; keep changes additive (new flags, new
  modules); send general fixes upstream.
- **From Phase 4: decide on a hard fork.** `parallel for` and the later language changes
  alter core semantics. From then on, port selected upstream fixes by hand.

## Risks

- **Upstream moves fast** (about 16 commits a day). The longer the soft-fork phase runs, the
  more each merge costs.
- **Phases 4 and 5 are research.** Their designs may need several attempts; the durations
  are the least certain here.
- **Ecosystem is not a coding task.** Libraries and users come after 1.0, over years.
