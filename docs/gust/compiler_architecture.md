# Compiler architecture: C, LLVM IR, or a backend of our own

**Recommendation: keep generating C, compile release builds with clang, and
make the C richer, rather than switching to LLVM IR or writing a backend.**
Add an LLVM IR backend later only if a measured optimization proves
impossible to express in C. The migration path below is incremental and
keeps every step shippable.

The comparison is grounded in Gust as it is today (October 2026) and in
measurements on this repository's programs; numbers are from one Windows
machine (Intel i7-11700F) and labelled as measured.

## What each part does today

```
 source ─► Gust front end ───────────────────────────► C ─► C compiler ─► machine code
           parse, resolve names, type check:             TinyCC, in process (gust run)
           roots and lifetimes, bounds-check             clang, gcc, MSVC, clang-cl (gust build)
           elimination, specialization per root;         + the runtime object, once per compiler
           optimize: inlining, folding, regrouping,
           tail loops; generate C (13,552 lines)
```

- **Gust's front end owns the language.** Every safety guarantee (lifetimes,
  bounds, flat-only thread messages, `return ... from`) is proved here,
  before any C exists. The C it writes is already-checked code with explicit
  bounds checks where they remain. Changing the backend changes none of
  this; it is most of the compiler and all of what makes Gust Gust.
- **The C compiler is the optimizer and the code generator.** It does
  register allocation, instruction selection, vectorization, inlining across
  the generated functions, and the platform's calling convention and object
  format.
- **LLVM is clang's optimizer and code generator.** When Gust builds with
  clang, its C goes through the same LLVM that Rust, Swift and Zig's release
  builds use. That is why, with clang, Gust's float-heavy benchmarks compile
  to nearly the same machine code as Rust's (measured: n-body 0.162 s against
  Rust's 0.161 s, ray tracing 0.0833 s against 0.0829 s).

## The three options

| | **C, compiled by clang** (today, improved) | **LLVM IR, through LLVM's API** | **A backend of our own** |
|---|---|---|---|
| Runtime performance | Rust parity on compute, measured; ahead where Gust's layouts win | the same LLVM; at most what metadata C cannot carry adds | far below for years: Goose's TinyCC path, a simple backend, ran n-body 11x slower than clang (measured) |
| Compilation speed | front end 0.06 s of a 1.0 s release build; clang's optimizer and the link are the rest (measured below) | skips parsing the C, 0.12 s of that 1.0 s; the optimizer and link remain | can be very fast (Go, Zig's self-hosted x86 backend) |
| Optimization opportunities | anything C can say: `restrict`, `__builtin_assume`, vector types, `musttail`, `#pragma` loops, per-function targets | also `noalias` scopes, range and alias metadata, the pass pipeline itself | whatever we build |
| Debugging | C-level today; `#line` directives would give source-level stepping and profiles at almost no cost | full DWARF / CodeView from Gust's own types | we would write the debug info |
| Diagnostics | Gust's own; a C compiler error is a compiler bug | the same | the same |
| Portability | every platform with a C compiler, and three compilers on Windows | LLVM's targets, LLVM's version per build | each target written by hand |
| ABI and C integration | free: the C compiler implements the ABI, and C headers can check prototypes (`c_headers`) | the ABI's lowering rules done by us (struct passing differs per platform) | all of it by us |
| Distribution | `gust` is 4.25 MB with TinyCC; release builds need clang (an install, or bundled) | LLVM embedded: +70 MB (`LLVM-C.dll`) or more | small |
| Maintenance | stable: C does not change | LLVM's C++ API changes every six months, its C API lags | permanent, large |
| Effort from here | weeks per improvement | months for parity with today's backend | years for parity with clang |

## Why not a backend of our own

Its benefit is compile speed and independence; its cost is years of work
before the code it produces approaches clang's. Goose's own data shows the
gap: its in-process TinyCC backend, which does no optimization, runs n-body
11x and ray tracing 14x slower than clang does (measured). A project of
Gust's size gets its speed from its memory model and from what the front end
proves; spending the next years on register allocation would stall the
language. If compile speed for development becomes the bottleneck, a fast
non-optimizing path already exists (TinyCC) and a better one is available
off the shelf (MIR, below).

## Why not LLVM IR yet

LLVM IR would remove the C step and let Gust attach metadata C cannot
carry: alias scopes for references whose roots differ, value ranges from
index types, exact control of the pass pipeline. But:

- **The measured gains so far came from the front end, and C carried them.**
  The n-body prototype that ran 2.1x faster than Rust was C with vector
  types; the toolchain fix that closed the 17-27% gap was choosing clang.
  Nothing measured yet needed IR.
- **The `simd fn` investigation is the closest case**, and C still had the
  answer: clang's vectorizer would not vectorize the loop in a standalone
  copy, and explicit vector types in the C fixed it.
- **The costs are concrete:** 70 MB or more added to every compiler
  download; LLVM's API changes every six months; Windows builds lose MSVC
  and gcc as choices; and the backend's ABI lowering (how structs pass on
  each platform) becomes Gust's code instead of the C compiler's.

## The recommended path

Each step is shippable on its own, measured against the benchmarks before
and after, and keeps C as the one backend.

1. **Clang by default** (done for Windows): `gust build` finds clang where
   the LLVM installer puts it. Next: make release archives say which clang
   they need, or bundle one.
2. **`#line` directives in the generated C**, mapping each statement to its
   `.gust` line, so debuggers, profilers and sanitizer reports point at Gust
   source. Days of work, in `src/codegen*.h`.
3. **`restrict` where roots are disjoint.** The front end specializes each
   function per root; where two reference or slice parameters have roots
   that cannot overlap, it can say so in C. This is Rust's no-alias
   advantage without Rust's exclusivity rules.
4. **Range facts as `__builtin_assume`** from index types and the
   bounds-check analysis, so the C compiler can drop checks it currently
   cannot prove away itself.
5. **The lane vectorization pass** (ROADMAP Phase 5), emitting explicit
   vector types, with `simd fn` choosing the vector width per CPU.
6. **MIR for `gust run`**, replacing TinyCC: an optimizing JIT (Lobster's
   MIR mode ran within 1.2-1.4x of its own compiled C++, against 2.1-2.4x
   for its TinyCC mode, measured), and MIT-licensed, which ends the LGPL
   relinking obligation TinyCC carries.
7. **Only then, and only if a measured case needs it: an LLVM IR backend**
   beside the C one, behind a flag, reading the same lowered form the C
   generator reads, so the two can be compared program by program. Its entry
   criterion is a benchmark where the best C Gust can write loses to what IR
   metadata would allow.

## Measurements

Compile times for the eight-benchmark program (`bench/goose/main.goose`
beside this repository, 549 lines of Gust, which become 5,775 lines and
241 KB of C), against the same benchmarks in Rust (679 lines). Measured
on 25 October 2026: Windows 11, Intel i7-11700F, clang 22.1.8, MSVC 19.50,
rustc 1.96.0; each command once to warm the caches (the runtime object is
built once per compiler and reused), then seven times, median wall time.

| what | median | range |
|---|---:|---:|
| `gust check`: parse, resolve, type check | 0.056 s | 0.050-0.065 |
| `gust -o main.c`: all of the front end, the optimizer and C generation | 0.062 s | 0.059-0.071 |
| `gust run`: the above, then TinyCC in process (no benchmark run) | 0.090 s | 0.079-0.108 |
| `gust build --cc clang`: a release build, `-O2` | 1.008 s | 0.945-1.215 |
| `gust build --cc clang --debug`: `-O0`, debug information | 0.838 s | 0.792-0.904 |
| `gust build --cc msvc`: `/O2` | 0.979 s | 0.893-1.203 |
| `rustc -C opt-level=3` | 1.000 s | 0.949-1.047 |
| `rustc -C opt-level=0` | 0.451 s | 0.419-0.478 |

The clang build of the same C, step by step:

| step | median |
|---|---:|
| parsing and checking the C (`clang -fsyntax-only`) | 0.119 s |
| compiling it at `-O0` (`-c`) | 0.153 s |
| compiling it at `-O2` (`-c`) | 0.538 s |
| linking it with the runtime object | 0.254 s |

What these say for the choice:

- **The front end is 6% of a release build.** Gust's own work, everything
  that makes the program safe and fast, takes 62 ms; the rest is the C
  compiler's optimizer (about 0.42 s), the link (0.25 s) and parsing the C.
- **Generating C costs 12% of a release build.** Parsing 241 KB of C is the
  step an LLVM IR backend would skip: 0.12 s of 1.0 s. The optimizer and
  the link remain whatever the backend is, so IR would make release builds
  at most about an eighth faster, which does not pay for the costs above.
- **The development loop is already faster than Rust's**, through TinyCC:
  `gust run` compiles and starts the program in 0.09 s, against 0.45 s for
  an unoptimized rustc build. A debug build through clang (0.84 s) is slower
  than rustc's, most of it in the link and debug information, not the C.
  MIR (step 6) is where the development loop improves next; a backend of
  our own would chase what TinyCC already gives.
- **Release builds are at parity**: 1.0 s for Gust through clang or MSVC,
  1.0 s for rustc at `opt-level=3`, on equivalent programs.

These are one machine's numbers, on one program of about 550 lines; a
program ten times the size has not been measured, and C compile time grows
with the C, which grows with specializations.
