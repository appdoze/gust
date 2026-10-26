# Gust roadmap

From fork to a 1.0 release that is faster than Rust across `compare/`, at least as safe, and
usable with large libraries and multiple cores. The durations assume **one experienced
developer working full time with AI coding agents**; a small team can run some phases in
parallel (3 with 2, 6 with 5). They are estimates, not measurements.

The performance and safety targets here are **goals**, not results. Passing selected
benchmarks does not establish that Gust is faster than Rust in general, nor that its checks
make it as safe as Rust; each phase states what was measured, where and how, and an item is
marked done only with the test or run that shows it.

"The fork" below is Gust. Every phase ends with exit criteria
checked by three suites: Goose's own tests (`python test/run_tests.py`, 3,551 files), the
cross-language benchmark harness (`python compare/run_bench.py`: checksums must stay identical
and nothing may get slower) and the safety probes (`python compare/safety/run_safety.py`).
Both are in `compare/` since Phase 0, and CI runs both with `--check`.

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
  Apache-2.0 too). **(done: `compare/`, with its provenance in `compare/README.md`; both
  harnesses run on Windows, Linux and macOS, and `--check` makes each a pass or fail: every
  Gust and Rust build agrees on every checksum, and every Gust probe does what
  `compare/safety/expected.json` says in release, `--checked` and `--debug`)**
- Commit the clang-discovery fix as the first change. **(done)**
- CI on Windows, Linux and macOS running Goose's tests plus `compare/` and `compare/safety/`.
  **(partly done: Goose's suite, the samples under `--checked` and a Linux ASan and UBSan
  profile run on ubuntu-latest, macos-latest and windows-latest; green at commit `420714c5`,
  run 40341927278, which includes the `net` tests and the network examples; `compare/`'s
  checks are added to it with the commit that adds `compare/`, not yet run there)**
- Record today's numbers (`compare/results.json`) as the baseline every later phase is
  measured against. **(done: `compare/results.json`, 26 October 2026, Windows 11 on an
  i7-11700F with clang 22.1.8, MSVC 19.50, rustc 1.96.0 and Lobster, nine configurations, 5
  launches each, best of 5 repeats, every checksum agreeing; other programs kept the
  machine 18-35% busy. `compare/safety/results.json` beside it)**

**Exit:** CI green on three platforms **(met for the suite, last at `420714c5`, run
40341927278; to be met again for each pushed milestone)**; baseline stored **(met)**.

## Phase 1: Quick wins (weeks 2–4)

- **clang as the default C compiler** wherever it is installed. **(done: on Windows
  since the fork, including CMake packages under clang, and now also in a developer prompt,
  where a `cl` on PATH used to win; on Linux, macOS and the BSDs discovery now takes `clang`
  before `cc` and `gcc`, where it used to take `cc`, which is gcc on most Linux systems. The suite's default-toolchain check asserts clang wherever clang is
  installed; its first hosted run is with that commit)**
- **`--checked`**: an optimized build that keeps the overflow, range and tag checks
  `--debug` has. **(done)** Measured cost on the 8 benchmarks: 0-1% on four, 6-7% on sort
  and astar, 12% on hashmap, 17% on bintrees, about 5% on average; still level with or
  ahead of unchecked Rust on all eight. Stays opt-in until Phase 2's range analysis
  removes more of the checks.
- **`extern fn` audit**: `--externs` lists every `extern fn` outside the standard library,
  `--deny-externs` refuses them, the equivalent of auditing Rust's `unsafe`. **(done)**
  `--externs` also reports each struct whose Gust and C layouts differ, with the `pad`
  that fixes it, and a sidecar's `c_headers` lets the C compiler check every prototype
  against its header **(done; `docs/gust/checked_and_externs.md`)**. CI builds and runs
  every sample with `--checked` on all three platforms, which found two samples
  truncating floats with `as` **(done)**.
- **`simd fn`**: its AVX2 / AVX-512 copies can be *slower* than the plain function
  (measured: 0.160 s against 0.105 s on the nbody lane prototype). Investigated: the cause
  is not the per-function `target` attribute; clang's SLP vectorizer does not vectorize
  the loop in a standalone copy at all, with or without the attribute, at any threshold,
  and only did in the plain version after inlining it into its caller. Writing the lanes
  as explicit vector types fixed it (0.077 s), so the fix belongs with Phase 5's lane
  pass. Until then, put `simd` on the function the hot loop is inlined into.
- **Generation checks for `reusable` pools**, so a stale reference aborts instead of
  reading another value (safety probe 9). **(done for debug builds: in `--checked` and
  `--debug`, a `let` or `var` reference rooted in a reusable pool aborts where it is used
  after its slot was freed, through a per-thread table of freed slots' generations, with no
  change to how references or pools are laid out; probe 9 now aborts in both, tested by
  `test/runtime/pool_ref_*.goose` and `compare/safety/`. Generation-checked handles,
  `stdlib/slots.goose`, check stored handles in every build. Not checked: references
  held in fields, elements, captured variables and parameters, relative links, plain
  indices, and release builds, which keep the spec's type-safe reuse)**
- Send the general fixes (clang discovery, CMake packages under clang) upstream as pull
  requests while the fork is still close to Goose.

**Exit:** a test shows `--checked` catching overflow in an optimized build **(done)**; no
benchmark regresses **(measurable from now: `compare/results.json` is the baseline)**;
generation checks for
pools land **(handles in every build; references in debug builds)**. The `simd fn`
criterion moves to Phase 5.

## Phase 2: Code quality (months 2–3)

- Emit **`restrict`** where the compiler has proved two references have different roots:
  Rust's no-alias advantage without its borrowing rules.
- Emit range hints (`__builtin_assume`) from index types (`u32 in a`).
- Close the bounds-check gaps in Goose's spec (Appendix B, 0a and 0f): indices loaded from
  arrays, disjunctive loop exits, `u64` ranges. 46 of 161 index checks remain in the `compare/` programs.
  **(In part: a self-recursive function now enters with its call sites' facts when its own
  calls keep them, by induction on the depth of recursion; a variable that steps one way
  inside a loop keeps its entry value as a bound; and a second invariant round proves what
  the first makes provable. That removes every check in quicksort's partition loop: 42 of
  161 remain, and sort runs in 0.190 s against Rust LTO's 0.218, best of five runs each on
  26 October, where it was level.)**
- Find out why Lobster (same author) runs `interp` 1.24x faster than Goose under clang.
- Bundle clang (or an LLVM toolchain) with releases, so users without it don't get MSVC's
  17–27% slower code.

**Exit:** goose/clang strictly faster than Rust LTO on all 8 benchmarks; `interp` at or below
Lobster's 0.419 s. **(Baseline, `compare/results.json`: gust/clang 0.166 against 0.174 s on
nbody, 0.083 against 0.088 on raytrace, 0.226 against 0.246 on astar, and 1.4-16x faster on
bintrees, hashmap and strings; level on interp, 0.569 against 0.569, and sort, 0.230 against
0.229, so the criterion is not met on those two. Measured flags for clang on the same C:
`-O3` alone changed nothing; ThinLTO across the program and the runtime gained 11% on sort,
6% on hashmap and 4% on astar in an interleaved run, through no runtime call in the hot
loops, so most likely layout, and needs lld; not made the default. After the bounds-check
work, 26 October, best of five runs each, gust/clang against rust/o3-lto: sort 0.190 against
0.218 and interp 0.506 against 0.551, ahead on both now; nbody 0.160 against 0.160 and
raytrace 0.082 against 0.083, level, so "strictly faster on all 8" is still not met, nor
interp at Lobster's 0.419.)**

## Phase 3: Library access (months 3–5)

- **C callbacks**: pass an `export fn` to C as a function pointer, with C's `void*`
  user-data mapped to a typed reference. No closure objects, so the language's rules hold.
  **(Done, with the user data an integer rather than a typed reference: `c_callback(f)`
  gives the C address, the callback runs above every data stack its caller holds, and may
  grow storage outside it but not shrink it or rebind an optional of it; qsort with a Gust
  comparator, and callbacks nested through C, in `test/packages/c_callbacks.goose`.)**
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
  place. The API example (`docs/gust/backend_api.md`) needs it for any shared data not
  known at compile time.
- **Addressed mailboxes**: replies a worker can wait for itself. (A **non-blocking
  `thread_wait`** would also save a thread per connection in a thread-per-connection
  server; measured, it halves that server's memory but not its tail latency, and the
  API example's pool does not need it.)
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
  Rust grew through with PyO3. **(Done: `scripts/gust_python.py` builds a library's
  exports into a ctypes package with checked integers, structs, slices read from
  sequences and buffers in place and written back, which pip installs as a platform
  wheel; tested on Windows and Linux in `test/python_bindings.py`, installed with pip and
  imported.)**
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
  HTTP server and client library on it. **(the first half done, without async: the
  standard library's `net`, `stdlib/net.goose` over `src/runtime/runtime_net.h`, with
  checked handles, thread ownership, a runtime that closes an ended thread's sockets, and
  `net::serve`, an event loop with bounded output, streaming and backpressure; tested in
  hosted CI on Windows, macOS and Linux, and under ASan and UBSan on Linux, run 40341927278;
  benchmarked on Windows only)**
- Started early, as `examples/api/`: an HTTP/1.1 JSON API served by a pool of
  event-loop workers (one per CPU), measured against a thread per connection and
  against Go (net/http, and Go with the same HTTP) in `docs/gust/backend_api.md`; it
  now stands on `net`, with responses of any size, and `examples/echo/` is a second,
  HTTP-free user of the loop. The backend roadmap there orders what follows; its next
  milestone is the network in hosted CI on three platforms (that criterion met, run
  40341927278) and measured at tens of thousands of connections, with epoll / kqueue /
  Windows AFD only where that shows `poll` costing **(both met locally: with 10,000 idle
  connections `poll` cut the measured load to a ninth on Linux and WSAPoll to an eighth
  on Windows; with epoll, kqueue and AFD polling the pool keeps 87% of its throughput at
  50,000 idle connections in WSL2 and 94% at 10,000 on Windows; kqueue awaits CI)**, then
  runtime-owned cancellation. M:N workers, work stealing and freeze-then-share stay milestones of
  their own.

**Exit:** an HTTP server and a callback-heavy GUI program written idiomatically; HTTP
throughput compared against a Rust (tokio) server in `compare/`.

## Phase 8: Hardening and 1.0 (months 16–24)

- Fuzz the compiler, the type checker and `from_bytes`; sanitizer jobs in CI **(a Linux ASan
  and UBSan profile runs already; with this milestone it also covers the network examples)**.
  **(Begun: `test/storage/fuzz_from_bytes.goose`, `test/stdlib/fuzz_json.goose` and
  `examples/api/fuzz_http.py` fuzz what reads untrusted input, 16 million images and 12.8
  million JSON texts under ASan and UBSan with no report, and found uninitialized enum bytes
  that `to_bytes` wrote out; `bench/compiler_fuzz.py` mutates the fixtures through the checker,
  code generation and a C compiler, and its first 200,000 programs found seven compiler faults,
  two of them crashes, all fixed.)**
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
