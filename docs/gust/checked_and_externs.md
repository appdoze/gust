# `--checked`, the `extern fn` audit, and CI

What Gust's two safety options guarantee, what they leave to C, and how CI
covers them. Checked on 2026-10-25 against Gust at `505d311d` plus the
changes this note describes.

## `--checked`

### What it guarantees

`--checked` builds an optimized program that aborts, naming the source file
and line and the values involved, instead of continuing with a wrong value,
on:

| check | example | normal build | `--checked` |
|---|---|---|---|
| signed overflow of `+ - *` and negation, at every width | `i64.max + 1`, `i8 100 * 2` | wraps | aborts |
| an `as` conversion that changes the value | `301 as u8`, `1.0e30 as i64` | wraps or truncates (defined, §6.3) | aborts |
| an enum tag no variant matches | (a corrupted fixed-mode value) | optimizer hint | aborts |

These are always checked, with or without `--checked`: array bounds,
division and remainder by zero, `INT_MIN / -1` at `i64`, a reusable pool's
`free` of a slot out of range, native stack overflow, and running out of
data-stack address space.

Never checked, by design: unsigned arithmetic (it wraps by definition, which
hashes and random-number generators rely on), `as!` (the unchecked
conversion, for when dropping bits is the point), shifts (the count is taken
modulo the width), and float arithmetic (IEEE 754: infinities and NaN).

### How it works, and where it reaches

`--checked` defines `GS_DEBUG=1` in the generated C, which is what `--debug`
defines too; `--debug` adds an unoptimized C build for a debugger, and
`--checked` leaves that out (`src/main.cpp`, the option parser). The define
goes into the C source rather than the C compiler's command line, so every
backend sees it. Each was tested:

| path | tested by | result |
|---|---|---|
| `gust run --checked` (TinyCC, in process) | driver check `run --checked (overflow aborts)` | aborts |
| `gust build --checked` (clang, MSVC, gcc) | driver check `build --checked (optimized, overflow aborts)` | aborts, and the C is built at `-O2` / `/O2` |
| a `thread_fn` worker, which is a program of its own | driver check `build --checked (overflow in a worker aborts)` | aborts the process, exit 1 |
| constant folding: a literal sum, a propagated constant, an inlined call | manual, five programs | the folder leaves overflowing operations to the checked code |
| Gust's own optimizer | `src/optimize_reassoc.h` | it regroups only integer `min` / `max` chains, which cannot overflow, and evaluates each operand once, in order |
| float to integer in a normal build | manual | defined by the spec (truncate, then wrap modulo 2^64; NaN is 0), and the generated code computes exactly that: `1.0e30 as i64` is `5076964154930102272` |

The test suite's 127 `runtime-debug` programs already run with `GS_DEBUG=1`
at the C compiler's `-O2` on every platform (`test/run_tests.py`,
`build_options`), which is what `--checked` produces; CI now also builds and
runs every sample with `--checked` (below).

The first such run found two samples relying on a silent conversion: the
ray tracer and the image filters scaled a float to a byte with `as`
(`(x * 255.0) as u8`), which drops the fraction, so `--checked` aborted with
`as conversion changes the value (debug): 140.25 as u8`. That is the spec's
rule (§6.3: `as` is range-checked, and a dropped fraction changes the value;
`test/expected/cast_abort_f32.stderr` pins `0.1 as i64`), so the samples were
wrong, not the check: they now say `as!`, the form for a conversion meant to
truncate, and print what they printed before.

### Limits

- **The C a program brings is not checked.** A package's or a program's own
  C (its sidecar's `c_sources`, a CMake package) and the runtime object are
  compiled without `GS_DEBUG`; so is any library an `extern fn` reaches.
- **The message says `(debug)`.** An abort reads `integer overflow (debug):
  ...` in a `--checked` build too: the runtime names the family of checks,
  not the build. The wording is shared with Goose's tests and runtime, so it
  stays.

### Cost

Measured with clang on the eight cross-language benchmarks (best of five
in-process repeats over six interleaved launches; the programs are not in
this repository yet, see ROADMAP.md Phase 0):

| benchmark | normal | `--checked` | cost |
|---|---:|---:|---:|
| nbody | 0.160 s | 0.162 s | 1% |
| raytrace | 0.082 s | 0.083 s | 1% |
| interp | 0.509 s | 0.508 s | 0% |
| sort | 0.202 s | 0.217 s | 7% |
| astar | 0.207 s | 0.220 s | 6% |
| bintrees | 0.031 s | 0.036 s | 17% |
| hashmap | 0.129 s | 0.145 s | 12% |
| strings | 0.042 s | 0.043 s | 1% |

About 5% on average. A checked build was still level with or faster than
Rust (which does not check overflow in release builds) on all eight. It
stays opt-in until range analysis removes more of the checks in loops.

## The `extern fn` audit

An `extern fn` is the one place a Gust program leaves the language's safety
rules: the compiler sees a signature, and C does what it does. The audit
separates what Gust checks from what it cannot.

### What Gust enforces

| property | how |
|---|---|
| only plain C shapes cross | rejected at the declaration: references or optionals inside a value, variable-size or resizable values by value, optionals, `varint`s, references returned (`TypeCheck::ExternValueOk`, tested) |
| writability | a `const T&` parameter is a pointer to `const` in the C prototype as well (fixed: it used to be written without `const`), and a read-only argument needs a `const` parameter |
| by-value pads to a shared library | an `extern "lib" { }` member may not take a struct with `pad` by value, which would arrive in the wrong registers (`DylibValueOk`) |
| the prototype, where C declares it | the generated C includes its headers before its extern prototypes, so a C compiler that sees the real declaration rejects a different one: libc's always, a program's own with `c_headers` (new) |
| who calls C | `--externs` lists every `extern fn` outside the standard library, and `--deny-externs` refuses them |

### What it reports but cannot enforce

`--externs` (after type checking) now prints, under each extern fn:

- **Layout differences.** Goose packs a struct's fields in order with only
  the pads written in it; C aligns each field naturally. For every struct
  that crosses by value, by reference or as a slice's elements, the report
  works out both layouts (`src/gust_ffi.h`) and names each difference with
  its fix: `Bad.b is at offset 1 in Goose and 4 in C: write pad 3 before
  it`, `Tail is 5 bytes in Goose and 8 in C: write pad 3 at its end where C
  keeps an array of them`. It assumes the C side declares the same fields
  without packing pragmas.
- **Pads passed by value to linked C**, which the C side must declare as
  bytes as well.
- **Prototypes nothing checks.** A shared-library member is bound by name
  when the program starts; no C compiler sees its prototype.

### What remains C's obligation

| obligation | why Gust cannot check it |
|---|---|
| the C function has the declared signature | without a visible declaration (`c_headers`, `--include`, or a libc header), C links by name only |
| pointers are used only during the call | Goose passes borrowed references; C must not keep a `T&`, a slice's data or a builder after returning. Gust's lifetime rules end at the call |
| C never frees, reallocates or writes past Goose memory | references point into data stacks and static data, which C must not free; a slice gives C `data` and `len`, and C must respect `len` |
| memory C returns is valid as long as it is used | it comes back as a `u64` handle, read with `ffi::cstr` / `ffi::bytes`, which trust it |
| errors are values | C reports failure through return values or out-parameters; it must not `longjmp` or throw across Goose frames, which would skip the data-stack resets that free memory |
| thread safety | Gust's threads share nothing, but C's globals are shared by every worker that calls it; a C library used from several workers must be thread-safe |
| no callbacks, no variadics | an extern fn cannot take a function pointer or `...`; a few lines of C adapt such an API |

### The recommended pattern

For C of your own, which is how Gust reaches sockets, databases and the
rest of the C world:

1. Give the C a header of plain signatures: integers, floats, `const
   uint8_t *` with a length, opaque `u64` handles for C's own objects, and
   C structs only behind those handles.
2. Include that header from the C (`#include "net.h"`) and name it in the
   sidecar (`c_headers = ["net.h"];`). The C compiler then checks the C's
   definitions against the header, and Gust's extern prototypes against the
   same header: a mismatch anywhere is a compile error, in both `gust build`
   and the in-process run.
3. Run `gust check --externs` and fix any layout line it prints for structs
   you do pass by reference.

Structs crossing by value or by reference can't be checked this way, since
the header's struct type is not Goose's (`Stats` against `Stats_g`): the
complete fix is for Gust to generate a header of its own types and
prototypes that the C side includes, as `cbindgen` does for Rust. That is on
the roadmap.

## CI

### The two cancelled runs

Both were cancelled by the workflow's concurrency rule (`group: CI-<ref>`,
`cancel-in-progress: true`), and neither hid a failure. Their logs were
readable with the repository owner's login.

| run | what started it | cancelled | why | state of its jobs |
|---|---|---|---|---|
| [39861874921](https://github.com/appdoze/gust/actions/runs/39861874921) | `workflow_dispatch` on `28366038`, started by hand because the push's run had not appeared yet | 22:03:36 | the push's own run for the same commit started its jobs at 22:03:37 | all four were still setting up |
| [39861945941](https://github.com/appdoze/gust/actions/runs/39861945941) | push of `28366038` | 22:10:16 | the next push (`696c6336`, run 39862886811) started | Ubuntu had passed; Windows had passed the whole suite and was in its VS Code step; macOS and the sanitizer job were mid-test. No log has a `FAIL` line |

The runs that superseded them passed on all four jobs (39862886811 and
39864469985).

### What changed

- **Every commit on the default branch now runs to the end.** Cancelling
  left `28366038` without a result from three of its four jobs. Superseded
  runs are still cancelled on other branches and pull requests.
- **Samples, checked.** Each platform's job now builds and runs every
  sample with `--checked` after the suite (`samples/run_samples.py
  --checked`, new): a check that fires on a correct program is a bug.
- **New driver checks** in `test/run_tests.py`: `--checked` in a worker,
  `c_headers` with a right and a wrong prototype, and the `--externs` layout
  report.

### What CI covers

| | Windows | Ubuntu | macOS |
|---|---|---|---|
| the full suite: parse, typecheck, 1,800+ rejection tests, generated C at `-O0` and `-O2`, the JIT, samples | yes | yes | yes |
| checked code: the `GS_DEBUG=1` programs at `-O2`, `--checked` driver checks, every sample `--checked` | yes | yes | yes |
| ASan + UBSan on the compiler and the C it generates | | yes | |
| the VS Code extension and language server | yes | yes | yes |
| a release build of every platform (`release.yml`, by hand or on a tag) | yes | yes | yes (x64 and arm64) |

Not covered: sanitizers on Windows and macOS (clang supports ASan there;
the suite's sanitizer profile is Linux-only today), and Linux on ARM.
