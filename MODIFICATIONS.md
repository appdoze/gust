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

- `parallel for i in n { ... }` and `parallel for i in a..b { ... }` (spec
  §6.5): the parser makes the body a nested function of the counter, which
  the loop calls (`parallel` is a word only before `for`, `src/parser.h`);
  `src/typecheck_parallel.h` (new) proves it race-free: it writes its own
  locals and the enclosing code's owned arrays only at its own index, reads
  those only there, writes no global or other variable around the loop, and
  calls nothing that does nor any extern outside the standard library.
  Codegen (`src/codegen_parallel.h`, new) fills a job with the free
  variables' arguments, starts up to `hardware_threads() - 1` workers with
  stacks of their own that share the caller's globals, and has every thread
  take chunks of the range (`src/runtime/runtime.h`); a JIT run takes them
  all on the calling thread, in order. The body is never inlined
  (`src/optimize.h`). `test/codegen/parallel_for.goose` (new) prints the
  same in a JIT run and in builds at -O0 and -O2; ten rejection fixtures in
  `test/errors_tc/` and two in `test/errors/` (new).
- `src/packages.h`: a sidecar may say `edition = "2026";`, the one edition
  so far (`docs/gust/stability.md`, new: what 1.0 will freeze, editions,
  versions and deprecation).
- `gust cimport header.h` (new, `src/cimport.h`, `src/main.cpp`): a C
  header as a package, parsed by the compiler's own TinyCC: an `extern fn`
  per function the header declares, a struct per struct its functions use,
  laid out as C's with explicit `pad`, and the header's integer and string
  macros and enum constants, each macro tried as a constant in a probe
  program (bisected past the ones that are not, `_Generic` keeping floats
  and pointers out); the C glue each extern calls, which passes pointers
  where Goose passes slices, a NUL-terminated copy of a `const char *`, a
  struct by reference, and calls the function by its parenthesized name
  past a function-like macro of the same name; and the sidecar, linking
  `--lib` and compiling `--source` C. Variadic and prototype-less functions,
  and types with no Goose form, are left out with the reason.
  `test/cimport.py` (new) imports a header of every shape and runs a
  program through it under the JIT and built; checked on zlib in WSL
  (compress and uncompress round trip, crc32 as Python's zlib computes it,
  the version from the function and the macro, all 80 of zlib.h's
  functions but the variadic gzprintf).
- `src/packages.h`, `src/main.cpp`, `docs/packages.md`: version ranges in a
  git source (`@^1.2`, `@~1.2.3`, `@>=1.2`, `@1.x`), resolved to the
  highest tag they allow with `git ls-remote`, and the program's lockfile,
  `<root>.lock`: each declared git package's source, ref and commit, which
  later builds take (a moved tag or branch in the shared cache leaves the
  program at its commit, fetched by itself where needed) until `gust fetch
  --update` resolves them again. Written atomically, and only for packages
  the root declares, not the blessed ones the compiler pins.
  `test/package_versions.py` (new).
- Labeled `break` and `continue` (spec §6.4, Goose's open item 13): `name:`
  on a loop or `block`, and `break :name e?` / `continue :name` from inside
  it at any depth (`src/lexer.h`, `src/parser.h`, `src/ast.h`). The checker
  binds a jump to the construct with its label, for its value, definite
  assignment and narrowing, and rejects an unknown label, a label two
  constructs around a jump share and a continue naming a block
  (`src/typecheck_flow.h`, `src/typecheck.h`, `src/typecheck_calls.h`,
  `src/typecheck_exprs.h`); codegen leaves through the named scope's
  restores (`src/codegen_stmts.h`, `src/codegen_nodes.h`); the bounds-check
  pass counts a labeled jump from a nested loop as one cutting its loop
  short (`src/bce.h`); clones, the optimizer's copies, the dump and the
  formatter keep labels (`src/clone.h`, `src/optimize.h`, `src/dump.h`,
  `src/format.h`). `test/codegen/labeled_jumps.goose`,
  `test/optimizer/bce_labeled_jumps.goose`, five rejection fixtures in
  `test/errors_tc/` and one in `test/errors/` (new); `bench/bce_fuzz.py`
  generates labeled jumps out of nested loops.
- `src/typecheck.h`, `src/typecheck_calls.h`: a nested function passed as a
  function value into a generic whose type parameter has the same name as
  one of the nested function's enclosing generic (`S` in both) had its
  parameter types read with the callee's binding of the name, and failed to
  match its own arguments; the names in a nested candidate's parameter
  types now resolve where it was declared. `test/typing/
  nested_fnval_generic_names.goose` (new).
- Compiler faults that `bench/compiler_fuzz.py` (new) found by mutating the
  fixtures and samples: a function value standing alone as a statement
  (`F;`) passed the checker and crashed code generation (now an error,
  `src/typecheck_flow.h`); a variant type of a scalar or struct
  (`f64.Circle`) crashed the checker at a call before the signature was
  validated (now an error in name resolution, `src/resolve.h`); a `return`
  with a value in `main` gave it a result its C `main()` could not pass
  (now an error, `src/typecheck.h`); and a `return`, `break` or `continue`
  where a value was wanted, an array element or a let's initializer, failed
  in code generation (now the statement it is, `src/codegen_construct.h`,
  `src/codegen_nodes.h`, with the type its context wanted kept on the node:
  `src/ast.h`, `src/typecheck_nodes.h`, `src/optimize.h`,
  `src/codegen_values.h`); an array size past §10.4's 2^48 reached the C
  compiler, which refused it (now an error, `src/typecheck.h`); and a data
  stack held across a call into a recursive cycle, as a dispatch copying a
  variant that is not fixed-size does, was reported as internal (now says
  what to change, `src/codegen.h`). `test/errors_tc/fnval_statement.goose`,
  `test/errors_tc/main_return_value.goose`, `test/errors_tc/array_size_bound.goose`,
  `test/codegen/leaving_values.goose` (new).
- C callbacks: `c_callback(f)`, the C address of export fn `f`'s wrapper,
  for C to call back during an extern call (`src/builtins.h`,
  `src/typecheck_builtins.h`, `src/typecheck.h`, `src/ast.h`). The wrapper
  starts above every data stack its caller holds, which each call of an
  extern outside the standard library hands on in `gs_cb_base`
  (`src/runtime/runtime.h`, `src/codegen_calls.h`, `src/codegen_emit.h`);
  the stack count takes such a call as one to the neediest callback
  (`src/codegen_stacks.h`), and the wrapper refuses a thread the runtime did
  not start and nesting past what was reserved. Such an extern counts as
  calling every callback: it reaches their globals (`src/codegen_frames.h`)
  and may change anything (`src/bce.h`). A callback may grow storage outside
  it but not shrink it or rebind an optional of it, which the suspended
  caller may rely on (`CheckCallbacks`). `docs/goose_spec.md` §7.11,
  `docs/reference.md` §24; `test/packages/c_callbacks.goose` (C folding an
  array through a callback, qsort with a Gust comparator, callbacks nested
  through C six deep, a bounds check kept across a call that moves an
  index) and three rejection fixtures in `test/errors_tc/` (new).
- `src/bce.h`: lengths across calls and joins. A body that resizes an array
  it is handed by reference has an exit summary, the facts between that
  length at its exits and its entry length and integer parameters, found
  callees first under the nonnegativity of the integer parameters it needs,
  which each call site proves before it gets them (`acc.push_n(v, n)` leaves
  `acc.len >= n`); an inlined call's reference parameter, a `let` reference
  to the caller's variable, is that variable's array; and an if whose one
  path changed a length joins to what both paths know of it. In `compare/`,
  nbody's parallel arrays and astar's score array lose their checks, 9 of
  51 left in `compare/gust/main.goose` where 17 were. Also, a length a call
  returns (`args().len`) lies in [0, 2^48]. `test/optimizer/bce_exit_lengths.goose`
  (new); `bench/bce_fuzz.py` generates helpers that resize through a
  reference and pushes through a `let` reference. Fixtures whose `bce:keep`
  the pass now proves are annotated `bce:elide` with the reason, and each
  keeps its point with a length nothing bounds (`test/optimizer/bce.goose`'s
  `guard_pop_unknown` and `pop_one_path`, a site that may pass an empty
  array, out-of-line bodies in `test/optimizer/bce_typed_kills.goose`).
- `src/bce.h`: bounds-check elimination proves three more kinds of index.
  A function whose only recursive calls are its own (quicksort) enters
  with what its other call sites prove about its parameters when, granted
  at entry, its own calls prove it too (induction on the depth of
  recursion; found by dry runs that mark nothing). A variable every write
  of which inside a loop is a non-wrapping step one way keeps its entry
  value as a bound across the loop, though it moves the other way outside
  it. And a second recording round proves the invariants the first
  round's make provable. In `compare/`'s sort, every check in the
  partition loop now goes. `test/optimizer/bce_loop_steps.goose`,
  `test/optimizer/bce_recursive_entry.goose` (new); `bench/bce_fuzz.py`
  generates the two shapes, with mutations that make some unsafe.
  `test/optimizer/basecase_copied_shrink.goose` also calls `f(0)`: with
  only `f(3)` its original base case is provably dead (1 <= n at every
  call) and its check is now elided, while the inlined copy keeps its own.

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
- `src/runtime/runtime_net.h`, `stdlib/net.goose`, `examples/api/http.gust`:
  a stop wakes every poller's wait at once, through a pipe in each epoll or
  kqueue set that the stop (a signal handler may) writes a byte to, and an
  empty completion posted to each AFD poller's port; each poller wakes once.
  `net::Ctl` has `stopping` and `deadline` (`Limits.request_ms` from a
  request's first byte), and HTTP's `Request` hands both to handlers.
  `test/threads/net_serve.goose` checks both.
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

## Third-party code

- `third_party/tinycc` (LGPL): `tcc_set_decl_func` reports each global
  declaration as it is parsed, a function, typedef, enum constant,
  object-like macro or struct layout per line with the file it is in, for
  `gust cimport` (`libtcc.h`, `libtcc.c`, `tcc.h`, `tccgen.c`, `tccpp.c`, each
  change marked `Gust:`). Nothing else in TinyCC changed, and nothing calls
  the hook but `gust cimport`.

## Scripts and tests

- `scripts/gust_python.py` (new): Python bindings for a library's exports,
  as a package pip installs (a wheel for the platform, any Python 3): the
  C built as a shared library, and a ctypes module that starts the runtime
  on import and checks each argument (integer ranges, struct fields,
  writability) before the call; structs, slices from sequences and buffers
  in place, written back to lists, and a returned `const u8[:]` as bytes.
  The header it reads gives each export's Goose signature and each struct's
  Goose fields in a comment (`src/codegen_emit.h`, `src/codegen_types.h`).
  `test/python_bindings.py` (new) runs it, imports the package directly and
  from where pip installed it; `docs/reference.md`.
- `compare/` (new): the cross-language benchmarks (Gust, Rust, Lobster;
  Lobster's suite and code under its Apache-2.0 licence, provenance in
  `compare/README.md`) and safety probes, with portable harnesses whose
  `--check` mode `.github/workflows/ci.yml` runs on every platform, the
  probes' expected outcomes (`compare/safety/expected.json`), and a
  measured baseline (`compare/results.json`).
- `test/net_fd_limit.py` (new), `test/run_tests.py`: a server out of file
  descriptors drops what it cannot take and does not spin (Linux, macOS).
- `bench/compiler_fuzz.py` (new): the compiler fuzzed for crashes, with the
  fixtures and samples mutated by tokens (mostly in kind: an identifier for
  another, a number, a type) and by lines; a program that checks clean goes
  on through code generation at -O2 and a C compiler's -fsyntax-only.
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
