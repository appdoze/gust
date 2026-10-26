# Modifications

Gust is Goose (https://github.com/aardappel/goose, Apache-2.0) with the
changes below. Every file that differs from Goose's carries a notice at its
top pointing here. The exact changes are the commits Gust adds to Goose's
history:

```bash
git remote add upstream https://github.com/aardappel/goose
git fetch upstream
git log upstream/master..main
```

## Toolchain

- `src/toolchain.h`: on Windows, also look for clang in
  `%ProgramFiles%\LLVM\bin`, where the LLVM installer puts it without adding
  it to PATH, before falling back to MSVC through vcvars64; the not-found
  message names that place. The default `GOOSE_HOME` is `~/.gust`.
- `docs/design/packages.md`: the not-found message as it now reads.

## Name

- `CMakeLists.txt`: the compiler builds as `gust` (`OUTPUT_NAME`); the CMake
  target keeps Goose's name.
- `src/main.cpp`: the help and the driver's messages call the command
  `gust` and source files `file.gust`; `gust version` prints
  `gust runtime <version>`. Imports resolve through `SourceFile`.
- `src/utils.h`: `SourceFile` and `IsSourceExtension`: a source path
  resolves to `.gust` where that file exists, else `.goose`.
- `src/builtin.h`, `src/packages.h`, `src/native.h`: package entries and
  native-part sources are found with either extension.
- `src/main.cpp`, `src/format.h`, `test/run_tests.py`, `test/format.py`
  (after Goose's command-line work): `gust version` prints
  `gust <version> (runtime <hash>)`, the version from `git describe`, which
  names Goose's nearest tag until Gust has tags of its own; the completion
  scripts (`gust completion`) complete `gust` and both `.gust` and `.goose`
  files; the man page is `GUST(1)`; `gust format` on a directory formats its
  `.gust` files as well as its `.goose` ones; the help and messages name
  `gust`.
- `README.md` is Gust's; Goose's is kept as `GOOSE.md`. `ROADMAP.md` and this
  file are new.

## Safety options

- `src/main.cpp`: `--checked` defines `GS_DEBUG=1`, the overflow, `as`
  range and tag checks of `--debug`, without `--debug`'s unoptimized
  debugger build: an optimized program that aborts on signed overflow
  instead of wrapping.
- `src/main.cpp`: `--externs` lists every `extern fn` outside the standard
  library and the packages compiled into the compiler (the C a program
  calls, which the safety rules cannot see into); `--deny-externs` makes
  each one an error.
- `src/gust_ffi.h` (new), `src/main.cpp`: `--externs` runs after type
  checking and prints, under each extern fn, where a struct it passes by
  value, by reference or as a slice is laid out differently by Goose (packed)
  and C (natural alignment), with the `pad` that fixes it; that a struct with
  pads passes by value; and that a shared library's prototype is unchecked.
  `--deny-externs` still refuses before type checking.
- `src/ast.h`, `src/packages.h`, `src/main.cpp`, `src/codegen.h`: a sidecar's
  `c_headers` (a path, or `<name>` for a system header) is included by the
  generated C before its extern prototypes, so the C compiler checks each
  prototype against the header's declaration.
- `src/codegen_calls.h`: an extern fn's `const T&` parameter is a pointer to
  `const` in its C prototype, as the header that declares it says. C that
  defines such a function in the generated file itself (`--include`) takes
  `const T *` (`test/extern_types.py` follows).

## Compiler

- `src/typecheck.h`, `src/typecheck_calls.h`: a nested function passed as a
  function value into a generic whose type parameter has the same name as
  one of the nested function's enclosing generic (`S` in both) had its
  parameter types read with the callee's binding of the name, and failed to
  match its own arguments; the names in a nested candidate's parameter
  types now resolve where it was declared. `test/typing/
  nested_fnval_generic_names.goose` (new).

## Standard library

- `stdlib/slots.goose` (new), `docs/stdlib.md`: generation-checked handles
  over a `reusable` pool: a handle names one allocation by index and serial
  number, and using it after the allocation is released, or releasing it
  twice, aborts in every build. `test/runtime/slots_*.goose` (new).

## Runtime

- `src/runtime/runtime_impl.h` (and `src/runtime_inline.h`, regenerated):
  on Windows and macOS, where the fault handler commits a data stack as it
  grows, a fault commits as much as the region has committed already, from
  16 KB up to 1 MB, instead of 1 MB every time. A program with many threads
  touches several stacks per thread, most a few bytes deep: with a thread
  per connection, the API example's commit charge per idle connection fell
  from 4.1 MB to 0.33 MB (Windows PrivateUsage).
- `test/runtime/runtime_commit_chunks.c`, `test/threads/stack_commit_growth.goose`,
  `test/ffi_stack_writes.py`, `test/threads/thread_contained_overflow.goose`
  (new): the commit sequence, sparse touches, the clamp at a region's end,
  release, system calls writing into data-stack memory, growth in workers,
  and that a data stack overrun ends the process even in a contained worker.
- `docs/implementation.md`: the commit chunk as it now is.

## Examples

- `examples/api/` (new): a JSON API over HTTP/1.1 (`api.gust`, `http.gust`,
  `jsonx.gust`, sockets and a poller in `net.c` behind `net.gust`), served
  by a pool of event-loop workers or a thread per connection; its Go twins
  (`go/server` on net/http, `go/rawserver` with the same minimal HTTP), a
  load generator (`go/loadgen`), behaviour tests (`test_api.py`), the
  benchmark and its measurement scripts, and raw results; see
  `docs/gust/backend_api.md`.

## Scripts and tests

- `test/api_example.py` (new), `test/run_tests.py`, `docs/testing.md`: the
  suite builds the API example normally and with `--checked` and runs its
  behaviour tests over sockets, against the pool and a thread per
  connection (baseline profile).
- `scripts/toolchain.py`, `scripts/check_doc_examples.py`: find a `gust`
  binary under `build/`, and a `goose` one from before the rename.
- `scripts/package_release.py`, `test/lsp_sweep.py`,
  `test/toolchain_selection.py`, `.github/workflows/ci.yml`,
  `.github/workflows/release.yml`: the binary is `gust`, and releases are
  named `gust-<version>-<platform>` and titled `Gust <version>`.
- `.github/workflows/*.yml`: the GitHub actions are on their Node.js 24
  versions (checkout, setup-python and setup-node v7, upload-artifact v7,
  download-artifact v7, configure-pages v6, upload-pages-artifact and
  deploy-pages v5), as GitHub retires Node.js 20 on its runners.
- `.github/workflows/ci.yml`: every commit on the default branch runs to the
  end (superseded runs are still cancelled elsewhere), and each platform runs
  the samples with `--checked`.
- `samples/run_samples.py`: `--checked` builds and runs every sample with
  `--checked`, which CI does on every platform.
- `samples/21_raytrace.goose`, `samples/22_image_filters.goose`: a float
  scaled to a byte converts with `as!`, the spec's form for a conversion
  meant to drop the fraction; with `as`, `--checked` aborts there, as the
  spec says it must (`140.25 as u8`). Output unchanged.
- `test/run_tests.py`: the driver checks expect the `gust` wording and test
  `.gust` sources (a program importing a `.gust` and a `.goose` module); the
  no-C-compiler check also hides `%ProgramFiles%` and `%ProgramW6432%`; new
  checks cover `--checked` (an overflow aborts, optimized, in a worker too),
  `--externs` / `--deny-externs` and their layout report, and `c_headers`.
- `test/errors_tc/covariant_store_*.goose`, `test/lifetimes/holder_stores_valid.goose`
  (new): the rust-lang/rust#164250 regression tests
  (`docs/gust/soundness_rust_164250.md`).
