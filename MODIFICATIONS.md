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
- `src/toolchain.h`, `docs/design/packages.md`: on Linux, macOS and the
  BSDs, discovery takes `clang` before `cc` and `gcc`, and on Windows the
  clang family before a `cl` on PATH, so clang is the default wherever it is
  installed. `test/native_toolchain.py`
  and `test/run_tests.py` check it.

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
  `gust`. `.github/workflows/ci.yml`: Goose's format check runs
  `build/gust`.
- `README.md` is Gust's; Goose's is kept as `GOOSE.md`. `ROADMAP.md` and this
  file are new.

## Safety options

- `src/codegen.h`, `src/codegen_stmts.h`, `src/main.cpp`: a `--debug`
  build's C marks each statement with its source line (`#line`), so that
  debuggers, profilers and sanitizer reports name `.gust` lines.
  `test/line_directives.py` (new), `test/run_tests.py`.

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

## Safety checks

- `src/runtime/runtime.h`, `src/runtime/runtime_impl.h`,
  `src/runtime/runtime_threads.h`, `src/codegen.h`, `src/codegen_values.h`,
  `src/codegen_nodes.h`, `src/codegen_stmts.h`, `src/codegen_builtins.h`
  (and `src/runtime_inline.h`, regenerated): in a debug build (`--checked`,
  `--debug`) a `let` or `var` reference rooted in a reusable pool of slots
  records its slot's generation, which `free` advances, and aborts where it
  is used after its slot was freed. `docs/goose_spec.md` 5.4 and
  `docs/reference.md` 14 say so; `test/runtime/pool_ref_*.goose` (new);
  `compare/safety/expected.json` holds probe 9 to it.
- `src/codegen.h`, `src/codegen_construct.h`, `src/codegen_values.h`,
  `src/codegen_nodes.h`: a fixed-mode ADT value built in a C local had the
  bytes its variant does not use (all of the payload, for a variant without
  fields) uninitialized, which `to_bytes` then wrote out: stack contents in
  a file or on a socket, and different images from `-O0` and `-O2`. They
  are zeroed now. Found by `test/storage/fuzz_from_bytes.goose`;
  `test/storage/serialize_variant_tails.goose` (new); `docs/goose_spec.md`
  §3.9 says so.

## Standard library

- `stdlib/slots.goose` (new), `docs/stdlib.md`: generation-checked handles
  over a `reusable` pool: a handle names one allocation by index and serial
  number, and using it after the allocation is released, or releasing it
  twice, aborts in every build. `test/runtime/slots_*.goose` (new).
- `stdlib/net.goose` (new), `docs/stdlib.md`: TCP over IPv4 with
  non-blocking sockets and a poller, every handle checked (kind,
  generation, owning thread), and `serve`, an event loop for a server with
  per-connection and per-worker limits, bounded output queues, a producer
  for responses of any size, timeouts, overload replies and a graceful
  stop. `test/stdlib/stdlib_net.goose`, `test/threads/net_serve.goose`
  (new).

## Runtime

- `src/runtime/runtime_net.h` (new), `src/runtime/runtime_ext.h`,
  `src/main.cpp`, `CMakeLists.txt` (and `src/runtime_inline.h`,
  regenerated): the sockets and poller behind `net`, part of the runtime
  object and the standalone runtime. Winsock is loaded from the system
  directory at first use, its functions checked against `winsock2.h` at
  compile time on clang and gcc. `src/codegen.h`: the C slice type of
  `i64` is emitted with the runtime's extern support, for `net`'s wait.
- `src/runtime/runtime_net.h`, `stdlib/net.goose`: out of file
  descriptors, an accept on Linux and macOS spends a reserved one to drop
  the waiting connection, and `net::serve` rests its listener after
  repeated failures (`Stats.accept_failures`) rather than spin.
- `src/runtime/runtime_net.h`: a poller waits with epoll on Linux, kqueue
  on macOS and the BSDs, and on Windows the AFD poll that Windows' own
  sockets are built on, through an I/O completion port (the technique of
  wepoll and Rust's mio), instead of `poll`/`WSAPoll`, which scan every
  socket on every wait; all level-triggered, behind the same calls.
  `GUST_NET_POLL=1` keeps the portable path, for testing it. A listener
  keeps an atomic tally of the connections accepted from it and still
  open, on every thread (`net::accepted_open`), and `net::serve` takes at
  most 32 connections a turn and honours `Limits.pool_conns`, a limit over
  every worker on the listener, which `examples/api/http.gust` uses in
  place of an even split of the pool's limit.
- `src/runtime/runtime_threads.h`: a hook a thread program runs at its end
  however it ends, which `runtime_net.h` sets at first use to close the
  network handles the thread still owns.

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
  `jsonx.gust`), served on the standard library's `net` by a pool of
  event-loop workers or a thread per connection, with responses built
  whole up to 1 MB and streamed (chunked) past that; its Go twins
  (`go/server` on net/http, `go/rawserver` with the same minimal HTTP), a
  load generator (`go/loadgen`, with idle keep-alive connections beside
  the measured ones), behaviour tests (`test_api.py`), the benchmark and
  its measurement scripts (`scale.py` for idle connections), and raw
  results; see
  `docs/gust/backend_api.md`. Its first version had its own sockets in C
  (`net.c`, `net.h`, `net.gust`, `api.build`), now removed.
- `examples/echo/` (new): a TCP echo server on `net::serve`, with no HTTP,
  and a `--check` mode that serves itself and checks the echoes.

## Scripts and tests

- `compare/` (new): the cross-language benchmarks (Gust, Rust, Lobster;
  Lobster's suite and code under its Apache-2.0 licence, provenance in
  `compare/README.md`) and safety probes, with portable harnesses whose
  `--check` mode `.github/workflows/ci.yml` runs on every platform, the
  probes' expected outcomes (`compare/safety/expected.json`), and a
  measured baseline (`compare/results.json`).
- `test/net_fd_limit.py` (new), `test/run_tests.py`: a server out of file
  descriptors drops what it cannot take and does not spin (Linux, macOS).
- Fuzzing the code that reads untrusted input:
  `test/storage/fuzz_from_bytes.goose` (from_bytes on mutated images of
  every serializable shape; whatever it accepts is walked in full and must
  serialize back), `test/stdlib/fuzz_json.goose` (the JSON parser and the
  streaming reader must agree, and a parsed tree must write out and parse
  back the same), each run briefly with a fixed seed as a fixture, and
  `examples/api/fuzz_http.py` (mutated requests against the API server,
  which must answer every one well formed without a worker aborting), run
  by `test/api_example.py` against each build; `test/fuzz_untrusted.py`
  (new) runs all three for millions of inputs under ASan and UBSan.
- `test/api_example.py` (new), `test/echo_example.py` (new),
  `test/run_tests.py`, `docs/testing.md`: the suite builds the API and echo
  examples normally and with `--checked` and runs their behaviour tests over
  sockets, the API's against the pool and a thread per connection; in the
  sanitize profile with the suite's sanitizer flags, as the fixtures are
  built.
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
