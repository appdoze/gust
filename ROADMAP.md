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
  **(done: Goose's suite, the samples under `--checked`, `compare/run_bench.py --check` and
  `compare/safety/run_safety.py --check` on ubuntu-latest, macos-latest and windows-latest,
  and a Linux ASan and UBSan profile; green at commit `2d46dd3c`, run 40404260948)**
- Record today's numbers (`compare/results.json`) as the baseline every later phase is
  measured against. **(done: `compare/results.json`, 26 October 2026, Windows 11 on an
  i7-11700F with clang 22.1.8, MSVC 19.50, rustc 1.96.0 and Lobster, nine configurations, 5
  launches each, best of 5 repeats, every checksum agreeing; other programs kept the
  machine 18-35% busy. `compare/safety/results.json` beside it)**

**Exit:** CI green on three platforms **(met, last at `2d46dd3c`, run 40404260948; to be met
again for each pushed milestone)**; baseline stored **(met)**.

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
  Rust's no-alias advantage without its borrowing rules. **(Not done: a slice crosses a
  call as a `{data, len}` struct, and clang derives no-alias only from a `restrict`
  pointer parameter, so this needs slices passed as two parameters, a calling-convention
  change; clang already versions the loops it vectorizes with a run-time overlap check.)**
- Emit range hints (`__builtin_assume`) from index types (`u32 in a`). **(Tried and not
  kept, 26 October: every index whose check the bounds-check pass elides, 68 in
  `compare/`, handed to clang as `__builtin_assume(i < len)`. In interleaved runs, best of
  30 each, the first moved hashmap 11% faster and nbody 4% slower, and the second, in the
  other order, put all eight within 2% (strings 4%): noise on this machine, no gain.)**
- Close the bounds-check gaps in Goose's spec (Appendix B, 0a and 0f): indices loaded from
  arrays, disjunctive loop exits, `u64` ranges. 46 of 161 index checks remain in the `compare/` programs.
  **(In part: a self-recursive function now enters with its call sites' facts when its own
  calls keep them, by induction on the depth of recursion; a variable that steps one way
  inside a loop keeps its entry value as a bound; and a second invariant round proves what
  the first makes provable. That removes every check in quicksort's partition loop: 42 of
  161 remain, and sort runs in 0.190 s against Rust LTO's 0.218, best of five runs each on
  26 October, where it was level. Then lengths across calls and joins: a callee that resizes
  an array it is handed by reference passes back what it leaves of the length, an inlined
  reference parameter is the caller's array, and an if that grows an array on one path
  joins to a bound. nbody's parallel arrays and astar's scores lose their checks: 9 of 51
  remain in `compare/gust/main.goose`, where 17 did, and 36 of 171 counting the standard
  library code it runs, with no change in time beyond noise (the checks nbody lost sit
  outside its inner loop). What is left: the dictionary's hash buckets, the heap's
  children, indices read out of data (`env[x.i]`, `closed[n]` for a node off the heap).
  Then the hash buckets: a loop index's invariant is judged against the shrinks of its own
  lifetime, from its declaration to the end of its block, rather than the whole body's,
  so an inlined probe keeps `i <= len - 1` where its caller sized the table first, and the
  dictionary's probes test for an empty table (`test/optimizer/bce_lifetime_invariants.goose`;
  600 programs of `bench/bce_fuzz.py`, no mismatch). 26 of 168 remain counting the standard
  library, 8 of them interp's `env[x.i]`, copied into each call by base-case inlining's
  match form.)**
- Find out why Lobster (same author) runs `interp` 1.24x faster than Goose under clang.
  **(Overtaken: with base-case inlining's match form (`src/optimize_basecase.h`), which gives
  a tree evaluator's leaf children no frame of their own, gust/clang runs interp in 0.436 s
  against Lobster's C++ build's 0.712 s and rust/o3-lto's 0.615 s on this machine, best of
  five alternating runs each with the same checksum, 30 October.)**
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
interp at Lobster's 0.419. Met since, on hosted runners: ahead of Rust LTO on all eight on
Ubuntu and on Windows (Phase 8's exit, run 40745431333), raytrace by the lanes' filter form
and interp by base-case inlining's match form; interp 0.37 s on Ubuntu and 0.29 s on
Windows there. On this machine interp is 0.436 s against Lobster's C++ build's 0.712 s,
measured side by side; the 0.419 s was a different Lobster build, not at hand.)**

## Phase 3: Library access (months 3–5)

- **C callbacks**: pass an `export fn` to C as a function pointer, with C's `void*`
  user-data mapped to a typed reference. No closure objects, so the language's rules hold.
  **(Done, with the user data an integer rather than a typed reference: `c_callback(f)`
  gives the C address, the callback runs above every data stack its caller holds, and may
  grow storage outside it but not shrink it or rebind an optional of it; qsort with a Gust
  comparator, and callbacks nested through C, in `test/packages/c_callbacks.goose`.)**
- **Automatic C header import** (`import c "sqlite3.h"`) using TinyCC's parser, with raw
  imports confined to auditable boundary modules. **(Done as a command: `gust cimport
  zlib.h --lib z` writes the boundary module, its C glue and its sidecar, parsed by the
  compiler's TinyCC through a declaration report added to it; the module is imported as
  any package and its externs are what `--externs` lists, rather than names a program
  gets from an `import c` line no audit sees.)**
- Prove it on four real libraries: SQLite, libcurl, zlib, OpenSSL. **(Done, on Linux:
  zlib in WSL, a round trip through compress and uncompress and crc32 as Python's zlib
  computes it; SQLite (326 functions, 13 the library does not define left out), libcurl
  (160) and OpenSSL's libcrypto (990, through a header including evp.h, sha.h, hmac.h
  and rand.h), each with nothing else left out, in `test/cimport_libs.py`, which CI
  runs: SQLite's prepared statements and sqlite3_exec's row callback, a libcurl fetch
  through its write callback with options set through curl_easy_setopt's variadic
  forms, SHA-256 and HMAC as Python's hashlib computes them. What they needed: variadic
  functions as forms taking one more argument, handles for a typedef of void and a
  `void *` without a length, pointer-constant macros, and a link check against
  `--lib`.)**
- **Licensing**: move TinyCC to a separate DLL (satisfies LGPL relinking) or plan its
  replacement by MIR in Phase 6. **(Done as an option: `-DGUST_TCC_SHARED=ON` builds
  TinyCC as `gusttcc.dll` / `libgusttcc.so` / `libgusttcc.dylib` beside the compiler,
  which the release script ships and a user can replace; tested on Windows (an
  in-process run and `test/cimport.py` through the DLL) and in CI on all three
  platforms. MIR, MIT-licensed, is also in now, behind `gust run --mir`, Phase 6.)**

**Exit:** a program using SQLite and libcurl with no hand-written `extern fn`; C libraries
that need callbacks (libcurl's write callback) work. **(Met on Linux, by
`test/cimport_libs.py`.)**

## Phase 4: Multicore (months 5–8)

Write the spec first; these change the language.

- **Safe `parallel for`**: workers read the enclosing scope without copying and each
  iteration writes only its own part of an output. The root analysis proves it race-free;
  no `Send`/`Sync`, no lifetimes. **(Done, additive (`parallel` is a word only before
  `for`): the body is a nested function the checker proves writes only its locals and
  the enclosing code's owned arrays at its own index; workers with stacks of their own
  read everything else in place; a JIT run runs the iterations in order. On a pool of
  workers the runtime keeps, which spin briefly between jobs before they sleep and see a
  job's start and report its end without a lock: against Rust + rayon in
  `compare/run_parallel.py` (16 hardware threads, others' programs running), nbody 0.041 s
  against rayon's 0.071 s (3.9x Gust's sequential run) and raytrace 0.0118 s against
  0.0126 s (7.3x), every checksum the sequential one bit for bit in both languages;
  ThreadSanitizer reports nothing on either or on `test/codegen/parallel_for.goose`.)**
- **Work-stealing runtime**, so loops balance themselves. **(In the simpler form: threads
  take chunks, about eight per thread, from one counter as they finish them, which
  balances uneven iterations; no per-thread deques. A parallel for inside another runs on
  its own thread.)**
- **Deterministic reductions**: the same floating-point result at any thread count.
  **(By construction for the form `parallel for` allows: each iteration writes its own
  element and the loop after it sums them in order, which no thread count changes. And
  written into the loop itself, `reduce s += e;`: the parser gives each iteration a slot
  of its own and adds the slots into `s` in order after the loop, bit for bit the result
  of the loop run in order; `test/codegen/parallel_for.goose`.)**
- **Freeze-then-share**: large read-only data built at run time, read by every worker in
  place. **(For a `parallel for`'s duration, done: its workers read the enclosing code's
  data and the globals in place, which the checker keeps every iteration from writing.
  For `thread_spawn` done too: `freeze(a)` copies an array of flat elements once into
  storage kept until the program ends and returns a read-only `const T[:]` of it, which
  is static data as a string literal's is; a `thread_fn` parameter may be such a view,
  and each spawn must pass static data there, so every worker reads the same bytes in
  place; `test/threads/freeze_share.goose`, clean under ThreadSanitizer.)** The API
  example (`docs/gust/backend_api.md`) needs it for any shared data not known at compile
  time.
- **Addressed mailboxes**: replies a worker can wait for itself. **(Done: `thread_self()`,
  `qsend(id, v)` and `qrecv<T>()`/`qrecv<T>(ms)`, a mailbox per thread and type beside
  the shared queue; `test/threads/mailboxes.goose`.)** (A **non-blocking
  `thread_wait`** would also save a thread per connection in a thread-per-connection
  server; measured, it halves that server's memory but not its tail latency, and the
  API example's pool does not need it.)
- **Zero-copy tree transfer**: structures linked by relative references cross a queue as one
  block, or by handing over the memory region (Goose's open item 9). **(As one block done:
  a queue element may be an array whose fixed-size elements link to one another by
  self-relative references; `qput` copies its bytes after checking them with `from_bytes`'s
  verifier, aborting where a link leads out of what is sent, and the receiver gets the
  same structure at its own address with no work per node;
  `test/threads/queue_tree.goose` and `queue_tree_outside.goose`. Handing over the memory
  region itself, without the copy, is not done.)**
- Add multithreaded versions of raytrace and nbody to `compare/`, with Rust + rayon
  equivalents, and new safety probes that try to race through `parallel for`. **(Done:
  `compare/gust/parallel.goose` and `compare/rust_parallel/` (cargo, rayon), run by
  `compare/run_parallel.py`, which CI runs; probes 11 to 13 race through a shared total,
  the next element and a callee's global, each a compile error in Gust and, written with
  scoped threads, in Rust. The tracer's spheres, walked by reference and only read, made
  the checker follow a `for &x` loop's element rather than count the loop as a write.)**

**Exit:** parallel raytrace and nbody at least as fast as Rust + rayon; every race probe is a
compile error. **(Met on this machine, with the numbers above.)**

## Phase 5: Vectorization (months 8–10)

- **Automatic outer-loop lane vectorization**: find loops whose iterations are independent
  (nbody's bodies), check legality, and run 2/4/8 of them per vector instruction while
  keeping each lane's floating-point order.
- Emit the lanes as explicit C vector types (`vector_size`), not hoping for clang's
  auto-vectorizer, which failed in `simd fn` copies (measured).
- Tie into `simd fn` dispatch for AVX2 / AVX-512 at run time.

**Status (26 October): measured, not built.** On an n-body force loop of 256 bodies in C,
clang 22 at -O2 -march=native on this machine: LLVM's own outer-loop vectorizer
(`-mllvm -enable-vplan-native-path` with `#pragma clang loop vectorize(enable)`) made it
twice as slow (0.61 s against 0.30 s); four lanes written as `vector_size(32)` doubles, with
`__builtin_elementwise_sqrt`, gave the same checksum bit for bit and ran in 0.135 to 0.20 s
against 0.23 s, 1.2x to 1.7x across runs with others' programs running. So the explicit-lane
route is the one, as planned. What it needs and does not have yet: the independence proof
for an ordinary `for` (the `parallel for` checker's rules, `src/typecheck_parallel.h`, apply
as they are), and a lane code generator that splits struct values (`V3`) into a vector per
field, broadcasts what every lane reads, and if-converts branches; that generator is the
phase.

**Status (27 October): built, exit met.** `src/codegen_lanes.h` runs a loop over an array's
elements four iterations at a time, one per lane of a `vector_size(32)` vector of doubles,
where its iterations are independent. The proof is the codegen's own: the body writes only
its own locals and the arrays it stores to at the loop's index, which nothing else in the
loop reads, and every other array it reads is distinct from them. Struct values become a
vector per field, what every lane reads is computed once and spread across the lanes, and
the rest of the loop runs one iteration at a time. A function holding such a loop gets the
`simd fn` versions for AVX2 and AVX-512, chosen at run time, with `mul_add` unfused.
Branches are if-converted: an `if` whose condition differs by lane runs both branches,
each lane keeping its own branch's values (`test/codegen/lanes_if.goose`). On
the unmodified `compare/gust/main.goose`, built by clang 22 at -O2, n-body runs in 0.098 to
0.107 s against rustc -O3 with fat LTO's 0.215 to 0.258 s, 2.1x to 2.5x run for run, with
the same checksum. That is 0.13 s with SSE2's lanes alone and 0.22 s one iteration at a
time. `test/codegen/lanes.goose` gives the same output to the last digit under TinyCC,
MSVC, MIR, clang and gcc, and `test/codegen/lanes_short.goose` the same abort. Only clang
builds the lanes: gcc 15's code for them ran n-body ten times slower than its own loop.

**Exit:** unmodified `compare/gust/main.goose` nbody at least 2x faster than Rust with an
identical checksum (the hand prototype reached 2.1x); `simd fn` copies never slower than
the plain function.

## Phase 6: Developer experience (months 10–13)

- **MIR JIT** for no-build runs (Lobster's MIR mode is 2.56x off the fastest build, against
  4.39x for Goose's TinyCC mode; MIR is MIT-licensed, ending the LGPL question).
  **(Begun: `gust run --mir` on Windows, Linux and macOS, `src/jit_mir.h`, MIR vendored
  from Lobster's copy into `third_party/mir`; TinyCC stays the default. On the compare
  suite in WSL it runs 1.54x off a native gcc build as a geometric mean (2.3x before the
  frame's places were registers, 1.94x before MIR's level 2), against TinyCC's 4.1x. The runtime is compiled into the compiler (`src/runtime_native.c`) and MIR
  compiles only the program's own C against it, which it finds among the compiler's
  exports: an empty program starts in 0.026 s, as under TinyCC, where c2mir compiling the
  runtime each run took 0.28 s, and c2mir reads no platform header, which it could not on
  Windows (MinGW's and MSVC's) or macOS (the SDK's stop at an unknown compiler). On
  Windows MIR also had to probe a frame larger than a page, as the stack grows a guard
  page at a time, and to generate every function before the run. What it took:
  a square-root instruction for MIR, c2mir copying small structs with moves and keeping
  `#pragma pack`, c2mir's float to unsigned 64-bit conversions made right from 2^63 up,
  and a correct size bound for one x86-64 pattern. `test/mir_runs.py` holds every fixture with
  an expected output to TinyCC's result, on all three platforms. Then small structs'
  fields in registers: c2mir keeps struct locals in the frame, and a pass before MIR's
  generator (`MirPromoteFrame`) makes each 8-byte place of it a register where nothing
  else can reach it, resolving every frame address from the definitions before it and
  giving away what an escaping address reaches (c2mir reports each frame object for
  that); n-body went from 6.5x to 3.3x off, raytrace from 4.4x to 1.8x. Then MIR's
  level 2 over those registers, once its address combining no longer folded a loop's
  phi copy into an address used past the loop (a latch that also exits the loop sets the
  copy on the way out), and with math's sqrt reaching c2mir as `sqrt` itself, which a
  call of a wrapper had kept a call whose every live double was saved around it on
  SysV: n-body 1.5x, raytrace 1.2x, interp 1.2x, sort 1.2x, astar 1.2x, hashmap 1.6x.
  Then the runtime's small memsets and memcpys made stores and loads at MIR's level
  (bintrees' zeroed links, the unaligned loads of a key's compare), and that compare,
  `gs_memeq`, inlined where c2mir calls it: strings from 2.0x to 1.4x, bintrees from
  2.9x to 2.5x. Measured as the fastest of five alternating runs in WSL, the geometric
  mean is 1.51x; hashmap (1.8x) and bintrees (2.5x) are furthest off, hashmap's hot
  loop calling nothing and bintrees' recursion each call's. On a hosted Ubuntu runner
  (`compare/ci_measure.py`, the fastest of seven alternating runs of each build, run
  40625482742) the geometric mean against gcc -O2 is 1.43x: n-body 1.52x, raytrace 1.56x,
  interp 1.06x, sort 0.99x, astar 1.50x, bintrees 2.11x, hashmap 1.62x, strings 1.40x. What
  is left in bintrees and hashmap is MIR's own code for a call and a probe loop: the same
  leaf-test inlining by hand ran bintrees' check 7% faster under MIR, against a native
  build twice as fast.)**
- **Python bindings generator**: turn a library into a pip-installable module, the route
  Rust grew through with PyO3. **(Done: `scripts/gust_python.py` builds a library's
  exports into a ctypes package with checked integers, structs, slices read from
  sequences and buffers in place and written back, which pip installs as a platform
  wheel; tested on Windows and Linux in `test/python_bindings.py`, installed with pip and
  imported.)**
- Formatter, a package registry with version resolution and a lockfile, a docs site.
  **(The formatter (`gust format`) and the docs site (`docs/site/`) came with Goose.
  Version resolution and a lockfile done: `@^1.2`, `@~1.2.3`, `@>=1.2` and `@1.x` ranges
  resolve to the highest tag they allow, and `<program>.lock` keeps each git package's
  ref and commit until `gust fetch --update`. A registry beyond the blessed table too:
  `import name from "@^1.2";` takes the repository from the first registry `GUST_REGISTRY`
  names (a directory or a git repository with an `index.txt` of names and sources) that
  lists it, through the lockfile as any git source; `test/package_registry.py`.)**
- Keep error output agent-friendly (`--diagnostics json` exists) and ship a compact
  language guide AI agents can load in one go. **(Done: `docs/gust/agent_guide.md`, the
  commands, the syntax, the memory model in five rules, the array family, references,
  functions and recursion, the errors an agent meets and their fixes, threads, `parallel
  for`, packages and the standard library, in one file whose examples the test suite
  compiles and runs.)**

**Exit:** a fork library installed with `pip` and called from Python; no-build runs within
1.5x of compiled speed. **(Both met: `test/python_bindings.py`; `gust run --mir` 1.43x off
gcc -O2 across `compare/` on a hosted runner, above.)**

## Phase 7: Expressiveness (months 12–16)

- From Goose's own open items: moves for resizable values, more than one resizable per
  struct, labeled `break`. **(More than one resizable per struct done as root structs:
  `root struct` holds any number of resizable fields, each stored apart as a variable or
  parameter of its own beside the owner (`src/roots.h`, spec §3.4.1,
  `docs/design/root_structs.md`), so each is a region of its own to the lifetime rules and
  the generated C is that of separate arrays; owners, `R&` parameters, bindings, generic
  roots, globals and workers' copies in `test/storage/root_structs.goose` and
  `test/threads/root_globals.goose`, 23 rejections in `test/errors_tc/root_*`. An
  ordinary struct's field `st: R&` holds an owner by reference, carrying each field's
  reference, reached through a name whose type is written; through such a struct the
  fields share one set of roots, so a shrink of one with a view into another live is
  rejected there: `test/storage/root_ref_fields.goose`, 17 rejections in
  `test/errors_tc/root_holder_*`.)** **(Moves done: `a.move()` gives an array's elements as a fresh
  value and leaves it empty, checked as `clear()` is (no view or reference into it may
  still be used) and known to the bounds-check pass as a clear; `a = a.move()` and
  elements with self-relative links are compile errors; `test/codegen/move_resizable.goose`
  and four rejections in `test/errors_tc/`.)** **(Labeled `break` and `continue` done, additive syntax that
  existing code never meets: `outer: for …` with `break :outer e?` and `continue :outer`,
  spec §6.4, through the checker's flow, codegen's restores and the bounds-check pass;
  `test/codegen/labeled_jumps.goose`.)**
- **Compile-time plugin registration** with `const for` (routers, event handlers resolved at
  compile time), instead of closures-as-objects. **(The router done, in the language as it
  is: `http::route<R, S>`, whose routes are the variants of an enum, each with an
  `endpoint` overload giving its method and path pattern and a `handle` overload; a
  `const for` over the variants expands into a test of each pattern, a `{name}` going into
  the variant's field of that name, with 405 naming the methods a path has as Go's
  ServeMux does. A generic library function reaching the program's own overloads per
  variant is the registration. `examples/api/api.gust` routes on it;
  `test/stdlib/http_route.goose`.)**
- **Thread-per-core async I/O**: each worker owns its connections and event loop; then an
  HTTP server and client library on it. **(Both done, without async: `stdlib/http.goose`, HTTP/1.1 over
  `net`, a client, and `http::serve`, the server `examples/api/` serves its JSON API with, moved into the
  library from the example.)** **(the first half done, without async: the
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
  50,000 idle connections in WSL2 and 94% at 10,000 on Windows; kqueue tested in macOS CI
  since run 40388485069)**, then runtime-owned cancellation **(begun: a stop wakes every
  waiting loop at once, and a handler sees its request's deadline)**. M:N workers, work stealing and freeze-then-share stay milestones of
  their own.

**Exit:** an HTTP server and a callback-heavy GUI program written idiomatically; HTTP
throughput compared against a Rust (tokio) server in `compare/`. **(The tokio comparison
done, beside the Go twins rather than in `compare/`, where the load generator and the
behaviour tests are: `examples/api/rust/`, the same minimal HTTP as `go/rawserver` on
tokio's multi-thread runtime, passing `test_api.py`; in one short run on Windows, mixed
load over 64 connections, Gust's pool served 168k req/s, Go with the same HTTP 140k and
tokio 88k, with tokio's tails the lowest (`docs/gust/backend_api.md` §5). A Linux run
is `.github/workflows/api-bench.yml`, run by hand on a hosted runner: there, at 64
connections, Gust 156k, tokio 146k, Go with the same HTTP 117k, tokio's p99.9 the lowest
and Gust's memory the smallest (run 40566316114). The GUI program is
`samples/32_ui_calc.goose`: every key, menu item and shortcut a command whose handler is
an overload on its variant, the dispatch, menu and shortcuts one `const for` over the
commands, with no callbacks kept as objects; the samples' runner plays it off screen.)**

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
  **(The brief is written: `docs/gust/soundness_review.md`, the claims and where each is
  enforced, what already tries to break them, the escape hatches and where to look first.
  The review itself needs reviewers. Found on the way: on hosted Windows under load, a
  data stack's commit fault inside the C library's vector memcpy came back with the
  thread's AVX state zero, storing 16 zero bytes of every 32 (`net_serve.goose`'s
  intermittent failure). Fixed by a scalar first touch of each page before any bulk copy,
  move or fill (`GS_TOUCHED`, `src/runtime/runtime.h`), with
  `.github/workflows/stress.yml` reproducing it without the fix and running clean with it.)**
- Freeze the spec; publish a stability and edition policy. **(The policy written:
  `docs/gust/stability.md`, what 1.0 freezes and what not, editions opted into in the
  sidecar, versions and deprecation. The freeze itself is the 1.0 tag's.)**
- Release 1.0 with a published benchmark and safety report (`compare/`, rerun).

**Exit:** no known soundness bugs; ahead of Rust on every benchmark in `compare/`; 1.0 tagged.
**(Ahead on all eight, with clang, Gust's default, on hosted runners: `compare/ci_measure.py`,
the fastest of seven alternating runs of each build, run 40745431333, Gust's time over
Rust's (rustc -O3, fat LTO). Ubuntu: n-body 0.26x, raytrace 0.93x, interp 0.66x, sort 0.96x,
astar 0.81x, bintrees 0.17x, hashmap 0.80x, strings 0.52x, geomean 0.55x. Windows: 0.26x,
0.84x, 0.56x, 0.90x, 0.86x, 0.06x, 0.85x, 0.60x, geomean 0.48x. raytrace and interp had been
level (1.01x and 1.04x on Ubuntu); the lanes' filter form and base-case inlining's match form
made them ahead. With gcc, raytrace (1.08x) and sort (1.15x) are still behind on Ubuntu.
No soundness bug is known with the Windows commit fix, but the review has not happened
(`docs/gust/soundness_review.md` keeps the list), and 1.0 is not tagged.)**

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
