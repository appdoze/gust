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
  variables' arguments and hands it to a pool of up to `hardware_threads() -
  1` workers the runtime keeps (`src/runtime/runtime_threads.h`), each with
  stacks of its own, which read the caller's globals; every thread takes
  chunks of the range (`src/runtime/runtime.h`), and workers spin briefly
  between jobs before they sleep. A JIT run, and a parallel for inside
  another, take every chunk on the calling thread, in order. A callee may
  take the enclosing code's storage for writing where it only reads it,
  which a fixpoint over the calls finds. The body is never inlined
  (`src/optimize.h`). `test/codegen/parallel_for.goose` (new) prints the
  same in a JIT run and in builds at -O0 and -O2; ten rejection fixtures in
  `test/errors_tc/` and two in `test/errors/` (new). Then: a callee's
  `for &x` loop over a parameter writes it only where the element is
  written through or handed on to a callee that writes it (a tracer reading
  its spheres by reference), and a write through a reference that may point
  anywhere writes every parameter it could; two fixtures more. The pool's
  workers see a job by its epoch and report it done by an atomic count,
  with a pause in their spin, the mutex taken only by a sleeper and a job's
  last worker. `compare/gust/parallel.goose`, `compare/rust_parallel/`
  (Rust with rayon) and `compare/run_parallel.py` (new) measure nbody and
  raytrace against rayon, which CI checks; `compare/safety/` probes 11 to 13
  (new) race through `parallel for`, each a compile error. Then `reduce x
  += e;` in the body (`src/parser.h`, `ParseReduce`): an iteration's slot of
  an array `parallel_reduce_slots` (`stdlib/std.goose`, which the parser
  imports for it) makes before the loop, added into `x` in order after it,
  the bounds evaluated once; two rejection fixtures in `test/errors/`. The
  slots and bounds carry plain names, `parallel_reduce_<k>_…`, so the
  dump of a program with a reduce parses again.
- Mailboxes (`src/builtins.h`, `src/typecheck_builtins.h`,
  `src/codegen_builtins.h`, `src/runtime/runtime_threads.h`): builtins
  `thread_self()`, `qsend(id, v)` and `qrecv<T>()`/`qrecv<T>(ms)`. A type's
  queue state keeps a list of mailboxes by thread id (`gs_qbox`), each a
  queue of its own made on first use; `qsend` packs as `qput` does into
  one, `qrecv` takes as `qget` does from the caller's. A `parallel for`
  body uses none of them (`src/typecheck_parallel.h`).
  `test/threads/mailboxes.goose`, `test/errors_tc/parallel_mailbox.goose`
  and `test/errors_tc/qsend_id.goose` (new).
- Frozen data (`src/builtins.h`, `src/typecheck_builtins.h`,
  `src/typecheck_calls.h`, `src/codegen_builtins.h`,
  `src/runtime/runtime_impl.h`): `freeze(a)` copies an array's or slice's
  fixed-size flat elements into storage the runtime never frees
  (`gs_freeze`) and returns a `const T[:]` of them rooted in static data. A
  `thread_fn` parameter may be a `const` slice of flat elements
  (`EnsureThreadSpec`), and a spawn checks that its argument there leads
  only to static data; the spawn packet carries the view as it does any
  fixed value. `test/threads/freeze_share.goose`,
  `test/errors_tc/thread_view_not_static.goose`,
  `test/errors_tc/freeze_references.goose` and
  `test/errors_tc/freeze_write.goose` (new); the editor grammars name it.
  Each copy is linked to the one before (`gs_frozen`, under the threads'
  mutex where there are threads), so that all stay reachable until the
  program ends and LeakSanitizer counts none lost.
- `a.move()` (`src/builtins.h`, `src/typecheck_builtins.h`,
  `src/codegen_builtins.h`, `src/bce.h`), Goose's open item 3: a member
  of the arrays that shrink, checked as `clear()` for its receiver and as
  `copy()` for its result; codegen builds the elements at the destination
  as `from_bytes` does (`OpenRzDest`), then clears the source (`EmitClear`,
  factored out of `clear`), and the bounds-check pass takes it for a clear.
  `test/codegen/move_resizable.goose`, `test/errors_tc/move_live_view.goose`,
  `move_live_ref.goose`, `move_self.goose` and `move_self_relative.goose`
  (new); the editor grammars name it.
- Linked arrays through queues (`src/typecheck_types.h`,
  `src/typecheck_builtins.h`, `src/codegen_builtins.h`): a queue element may
  be a fixed or grow-only array whose fixed-size elements hold only
  self-relative references to its elements (`IsLinkedImage`, `from_bytes`'s
  condition). `qput` and `qsend` run the type's `from_bytes` verifier over
  the image they send and abort where a link leads out of it; a queue
  receive is built where it is received (`BuiltWhereReceived`).
  `test/threads/queue_tree.goose`, `test/threads/queue_tree_outside.goose`
  (an expected abort) and `test/errors_tc/queue_link_not_element.goose`
  (new).
- `samples/32_ui_calc.goose` (new, with `data/ui_calc.args` and its
  expected output): a calculator on `ui` whose commands are an enum's
  variants with a `run`, `menu` and `key` overload each, dispatched and
  listed by `const for`; `samples/README.md`.
- `test/threads/net_serve.goose`: where the starved worker's bytes are not
  the pattern, which happened on a loaded Windows runner, it prints the
  first wrong byte of each connection and what came there.
- `test/mir_runs.py`: a failure shows the head of MIR's stderr, where its
  first error is, rather than the tail.
- `docs/gust/agent_guide.md` (new): the language for an AI coding agent in
  one file: commands and JSON diagnostics, syntax, the memory model in five
  rules, the arrays, references and optionals, functions, function values
  and recursion, the errors an agent meets with their fixes, threads,
  `parallel for` and `reduce`, packages, the standard library and C. Its
  examples are compiled and run by `test/run_tests.py`, as the reference's
  are; `README.md` links it.
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
  functions but the variadic gzprintf). Then: a variadic function as
  itself and six forms taking one more argument (`_int`, `_f64`, `_ptr`,
  `_str`, `_buf`, `_out`), a pointer to a typedef of void and a `void *`
  no length follows as `u64` handles, function pointers passed as `void *`
  (TinyCC's spelling of their types is not always C's), pointer-constant
  macros as `u64`, glue locals that no parameter name can shadow, and with
  `--lib` the functions linked against the library first, those it does not
  define left out. `test/cimport_libs.py` (new) uses SQLite (prepared
  statements, sqlite3_exec's row callback, an error message SQLite
  allocated and frees), libcurl (a fetch from a local server through its
  write callback, options through curl_easy_setopt's forms) and OpenSSL's
  libcrypto (SHA-256 and HMAC as Python's hashlib computes them) under the
  JIT and built, on Linux where their headers are installed; CI installs
  them.
- `gust run --mir` (new, `src/jit_mir.h`, `src/main.cpp`, `CMakeLists.txt`):
  the C an in-process run compiles (`JitSource`, now shared with TinyCC's
  `RunJit` in `src/jit.h`), compiled by MIR's c2mir and generated at its
  level 1 (its level 2 kept a stack-top pointer stale past a loop), each
  function as it is first called, its imports resolved from the packages'
  native functions and then the C library's (`SystemSymbol`,
  `src/process.cpp`); calls of sqrt and sqrtf become MIR's square root
  first, and a memset or memcpy of up to 32 or 16 bytes of a constant
  size, the runtime's small zeroing, copies and unaligned loads, becomes
  stores and loads of alias set 0 (`MirLowerCalls`), but not through an
  address of a register variable that anything else reads: bintrees runs
  about a quarter faster. A copy or a zeroing of the whole of a register
  variable through its `addr` or `addr32` becomes a mov of the register,
  and the `addr` goes; and `gs_memeq` is `static inline` under c2mir
  (`src/runtime/runtime.h`), which then inlines its calls: strings runs a
  quarter faster too. The runtime takes c2mir (`__MIRC__`) for a compiler without GCC's
  atomics, its SSE scan and epoll, as it takes TinyCC (`src/runtime/`).
  `test/mir_runs.py` (new) runs the fixtures with an expected output both
  ways and requires the same output and outcome. Then the runtime compiled
  into the compiler (`src/runtime_native.c`, new; `CMakeLists.txt`): the
  runtime object `--emit-runtime` writes, built with GS_NATIVE_RUNTIME,
  which exports what it defines (`GS_API`, `src/runtime/runtime.h`; the
  dynamic list `cmake/runtime_exports.list` on Linux, `SystemSymbol`
  searching the compiler's own exports on Windows), ends the process as
  an in-process run does (`gs_end`) and takes the resource directory at
  run time (`gs_native_resource_dir`, `src/runtime/runtime_os.h`). A
  `--mir` run compiles the program's unit as a build with a separate
  runtime writes it, and links `gs_rt_start_<version>` to that runtime's
  start. Under c2mir the unit declares the C library functions it uses
  rather than include the platform's headers (`runtime.h`), which c2mir
  could not read on Windows or macOS, and folded infinities and NaNs are
  `GS_INFINITY` and `GS_NAN` (`src/codegen_values.h`), clear of a
  package header's math.h. Every function is generated before the run:
  MIR's lazy thunk corrupted the first call on Windows. A run starts in
  about 30 ms rather than 0.3 s, and `--mir` runs on Windows.
  `test/runtime_text.py` (new) checks that `src/runtime_inline.h` is
  `src/runtime/` as it is.
- `GUST_TCC_SHARED` (new CMake option, `cmake/libtcc.cmake`,
  `CMakeLists.txt`, `scripts/package_release.py`): TinyCC built as a
  shared library beside the compiler (`gusttcc.dll`, `libgusttcc.so`,
  `libgusttcc.dylib`, found through the executable's directory) rather than
  linked into it, which a release then ships: what the LGPL's relinking
  terms ask of a binary distribution. Off by default.
- `src/packages.h`, `src/main.cpp`, `docs/packages.md`: version ranges in a
  git source (`@^1.2`, `@~1.2.3`, `@>=1.2`, `@1.x`), resolved to the
  highest tag they allow with `git ls-remote`, and the program's lockfile,
  `<root>.lock`: each declared git package's source, ref and commit, which
  later builds take (a moved tag or branch in the shared cache leaves the
  program at its commit, fetched by itself where needed) until `gust fetch
  --update` resolves them again. Written atomically, and only for packages
  the root declares, not the blessed ones the compiler pins.
  `test/package_versions.py` (new). Then registries: a source of `@ref`
  alone takes the package's repository from the first registry
  `GUST_REGISTRY` names whose `index.txt` lists it (`RegistrySource`), a
  directory read in place or a git repository cloned into
  `GOOSE_HOME/registry/` and fetched again by `--update`; the source it
  makes goes through the lockfile as any other. `test/package_registry.py`
  (new).
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
- Root structs (spec §3.4.1, `docs/design/root_structs.md`): `root struct`
  (`root` a word only before `struct`, `src/parser.h`; `src/dump.h`) holds
  any number of resizable fields, each stored apart. `src/roots.h` (new)
  lowers it after name resolution and before the checker. Each resizable
  field becomes a variable or parameter of its own named `owner.field`, the
  owner the struct of the other fields (`SStruct::rootfields` keeps the set
  aside, `Param::rootcomp` marks the parameters added). A call passes the
  owner and then each field, and `x.f(...)` becomes such a call. Every other
  use of a root value or type is an error there, the type positions found
  among `Ast::alltypes`. The checker is unchanged but for a message naming
  a field stored apart (`src/typecheck_exprs.h`), and for traces and hovers
  that leave the added parameters out (`src/typecheck.h`,
  `src/lsp_features.h`). Codegen names `a.b` `a_b_gr1` (`Sanitize`), a form
  no other name takes. `test/storage/root_structs.goose`,
  `test/threads/root_globals.goose` (new) and 23 rejections
  `test/errors_tc/root_*` (new).
- Root structs held by reference in fields (spec §3.4.1,
  `docs/design/root_structs.md`): an ordinary struct's field `st: R&`
  gains a reference field per field of R stored apart, `st.field`
  (`Field::rootcomp`, `src/ast.h`), which the struct's literal fills from
  the root value it gives `st` and `c.st .= p` rebinds (`src/roots.h`,
  `Hold`, `Lit`). `c.st` is a root value only where `c` is a name whose
  type the source writes as the struct's (`PathOf`); every other path to it
  is an error, in `src/roots.h` for a call passing it and in the checker's
  message for a field stored apart reached through it
  (`src/typecheck_exprs.h`). The language server leaves the added fields
  out (`src/lsp_features.h`, `src/lsp_analysis.h`).
  `test/storage/root_ref_fields.goose` and 17 rejections
  `test/errors_tc/root_holder_*` (new); `root_ref_in_field.goose` and
  `root_alias_field.goose` now reject what stays rejected (an `R&` in a
  root struct's field, an element of one), with
  `root_ref_element_field.goose` and `root_ref_optional_field.goose`
  (new).
- Lanes (ROADMAP Phase 5): `src/codegen_lanes.h` (new) runs a loop over an
  array's elements four iterations at a time, one per lane of a vector of
  doubles, where the iterations are independent: n-body's force loop. Each
  lane computes what its iteration does, so every result is the scalar
  loop's to the bit. The block of lanes is under `#if defined(GS_LANES)`
  (`src/runtime/runtime_ext.h`: `gs_l4d`, a `vector_size` type, with its
  splat and sqrt), which clang defines and gcc, MSVC, TinyCC and MIR do
  not (gcc's code for the lanes ran n-body ten times slower). The rest of the loop runs as it is. The hook is the array loop of
  `ForLoop::CgStmt` (`src/codegen_nodes.h`). A function with such a loop is
  emitted as a `simd fn` is, with AVX2 and AVX-512 versions, but keeps
  math's `mul_add` unfused (`EmitSimdVersions`' `fused`,
  `src/codegen_emit.h`). The aggregate hoist leaves the lane types'
  declarations in their block. Branches are if-converted: an `if` whose
  condition differs by lane runs both branches, each assignment into the
  lanes its branch is taken in, under a mask (`gs_l4m`), and nothing in
  such a branch may abort or store into an array.
  `test/codegen/lanes.goose`, `test/codegen/lanes_short.goose` and
  `test/codegen/lanes_if.goose` (new): the same output, and the same
  abort, one iteration at a time and in lanes.
- Smaller executables (`src/build.h`, `Builder::CFlags`, `LinkFlags`,
  `UcrtDll`): a release build compiles with a section per function and
  datum and links with the unreferenced ones left out (the runtime object's
  cache directory says so, `release-gc`); on Windows one that links no C
  package takes the universal CRT from Windows' own DLL and links vcruntime
  in (the hybrid CRT), unless `--static-crt` (`src/main.cpp`), so that it
  needs no Visual C++ redistributable. compare/'s program: 268 KB to 65 KB
  under clang and MSVC, with a working set 0.1 MB below that of vcruntime
  and the universal CRT both as DLLs. `test/run_tests.py` checks the
  universal CRT's import library and `--static-crt` in a dry run.
- Bounds-check elimination's loop invariants against a variable's own
  lifetime (`src/bce.h`, the candidates of `RunSpecOnce`): a candidate the
  whole body's kills rule out is judged again against the shrinks and
  re-bindings of the statements after its declaration in its block, which
  is where an invariant set at the declaration can break. An inlined probe
  whose caller sized the table first keeps `i <= len - 1`, and
  `stdlib/dictionary.goose`'s probes test for an empty table, so their
  loops lose every check. `test/optimizer/bce_lifetime_invariants.goose`
  (new).
- Base-case inlining's match form (`SetupMatch`, `src/optimize_basecase.h`):
  a recursive function whose body is one `match` over an enum's variants,
  some arms recursing and some not, has the arms that do not recurse copied
  to each self-call, with a wildcard arm making the call for the rest, so a
  child that is a leaf costs no frame. `compare/`'s interp runs a fifth
  faster under clang. `test/optimizer/basecase_match.goose` (new).
- The lanes' filter form (`PlanFilter`, `EmitFilter` in
  `src/codegen_lanes.h`; `CodeGen::GenLaneFilter`, hooked in beside the
  lanes in `ForLoop::CgStmt`): a loop of pure statements ending in an `if`
  tests four iterations at once and runs them as the loop does wherever any
  lane's tests hold, skipping the rest. Elements bound by reference and
  inlined functions that return early are lane values there.
  `gs_l4m_any` (`src/runtime/runtime_ext.h`). `compare/`'s raytrace runs
  27% faster under clang with the same checksum.
  `test/codegen/lanes_filter.goose` (new).
- Lanes under MSVC and gcc (`src/codegen_lanes.h`,
  `src/runtime/runtime_ext.h`): the block of lanes operates on `gs_l4d` and
  `gs_l4m` only through macros (`gs_l4d_add`, `gs_l4d_lt`, `gs_l4m_and`,
  `gs_l4d_blend`, `gs_l4d_get`, ...), which under clang are its vector
  types' operators, as before, and under MSVC and gcc on x86-64 inlined
  functions on a pair of SSE2 registers each, the same operation per lane.
  `compare/`'s n-body runs 44% faster under MSVC (0.11 s, against Rust's
  0.17) and 31% under gcc, with the same checksums; the lanes tests give
  their output under all three at -O0 and -O2.
- simd versions under gcc (`src/runtime/runtime.h`, `GS_SIMD`), but on
  Windows, where gcc's AVX spills misalign (gcc bug 54412), and in them
  lanes on AVX's 256-bit registers (`src/runtime/runtime_ext.h`, the
  `_avx` implementations, selected by `GS_L4_ISA`, which
  `CodeGen::EmitSimdVersions` in `src/codegen_emit.h` switches around each
  version). Under gcc `compare/`'s raytrace went from 1.07x Rust's cycles to
  0.89x and n-body from 0.57x to 0.50x (`compare/quiet_bench.py`), with the
  same checksums. `.github/workflows/ci.yml` builds the lanes, simd and
  float tests with gcc on Linux, optimized and `--debug`.
- simd versions under MSVC, which has no per-function target: `gust build`
  (`src/build.h`) compiles a program with versions a second time, with
  `/arch:AVX2 /DGS_TU_AVX2`, a unit holding only the AVX2 versions with
  external linkage, which the first unit's dispatch (`/DGS_TU_BASE`,
  `__cpuidex` and `_xgetbv`) calls (`src/runtime/runtime.h`, `GS_TU_SPLIT`,
  `GS_SIMD_BODIES`; `CodeGen::EmitSimdVersions`). Every datum the program's
  C defines and changes is declared `GS_DATA`, defined in the first unit
  and `extern` in the second (`src/codegen.h`, `src/codegen_builtins.h`,
  `src/codegen_calls.h`, `src/codegen_emit.h`, `src/codegen_stmts.h`), and
  a function takes the external stack block into a local
  (`GS_STKS_LOCAL`), which MSVC otherwise reloaded after every store
  (`compare/`'s astar, 6% slower). The AVX lane operations are macros under
  MSVC (`src/runtime/runtime_ext.h`): inlined as `__forceinline` functions
  at `/O2`, MSVC 19.50 read a lane struct's later members wrongly. Under
  MSVC `compare/`'s raytrace went from 1.20x Rust's cycles to 1.00x and
  n-body from 0.67x to 0.49x (`compare/quiet_bench.py`), with the same
  checksums. `.github/workflows/ci.yml` builds the lanes and simd tests with
  MSVC on Windows, optimized and `--debug`, each with its AVX2 unit.

## `cold fn`

`cold fn` (spec §7.15) marks a function that runs rarely. It is never inlined,
gcc and clang lay out a branch to it as the unlikely way, and what its call
needs saved is saved on that branch. A branch to an ordinary function is
assumed to be taken half the time, so an `if` that only guards a slow path
costs registers on the way past it: without this, the check the dictionary's
flood guard adds cost its hashmap benchmark 7%, and with it nothing measurable.
`test/syntax/cold_fn.goose`.

## `gust build --target`

`gust build --target x86_64-linux-musl` builds through Zig's C compiler, `zig cc`, a
clang that carries a C library for every target, so that one machine with nothing else
installed builds for any of them: a static Linux executable with no dependencies, arm64
Linux, Windows and macOS. Gust compiles to C, so this needed no backend of its own.

Against the usual dynamic glibc build on one runner a static hello world resides in 88 KB
rather than 1,964 KB (Rust 2,152 KB, Go 1,988 KB), of which the private part is 80 KB against
144 KB, as glibc's pages are shared between processes. It goes from start to exit in 0.17 ms
rather than 0.49 ms, and is 37 KB rather than 27 KB. The eight benchmarks ran no slower, and
each resides in up to 1.8 MB less, zig's clang being a newer one than that runner's. A
static executable cannot load libraries at run time. Workers get an 8 MB stack under musl as
under glibc, musl's default being 128 KB. `scripts/target_tests.py`.

## Safety checks

- `src/typecheck_parallel.h`: a `parallel for` body, or anything it calls, may not print or
  call a standard library function that changes files or uses a standard stream or socket.
  Such a body type-checked, as if the standard library's effects all stayed in their
  arguments, and did not do what the loop run in order does: 2,000 `append_file`s to one
  file left 5,701 to 7,639 of the 8,890 bytes, differently each run. Reading files stays
  allowed. `test/errors_tc/parallel_file_output.goose`, `parallel_file_output_callee.goose`,
  `parallel_print.goose`, `parallel_print_callee.goose`, `parallel_socket.goose` and
  `test/stdlib/parallel_read_files.goose` (new); `docs/goose_spec.md` §6.5 says so.
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

- `stdlib/http.goose` (new): an HTTP/1.1 client over `net`, `get`, `post`
  and `request`, one request per connection: a body by Content-Length, in
  chunks (decoded) or to the server's close, the status as the result, and
  net's errors or `PROTOCOL` below 0. `test/threads/http_client.goose`
  (new) against a server thread; `docs/stdlib.md`. Then the server too:
  `examples/api/http.gust`, moved into the module as `http::serve` with its
  `Request`, `Response` and `Counts` (the client's own head parser renamed
  `reply_head`), and a response's `content_type`, JSON by default;
  `examples/api/api.gust` names them through `http::`, and
  `test/threads/http_server.goose` (new) serves the client from a thread.
  Then `http::route<R, S>`, a router resolved at compile time: a `const for`
  over the variants of the program's enum `R` tests each one's `endpoint`
  pattern, binds `{name}` segments into its fields (`param`, for integers and
  inline strings) and calls its `handle`, or answers 405 with the methods
  the path has or 404. `Response.allow` is an inline string `u8[..64]`, so
  that a 405 can name more than one method. `examples/api/api.gust` routes
  on it; `test/stdlib/http_route.goose` (new). `examples/api/bench.py`
  gives the server and the load generator half the CPUs each where there
  are fewer than 16 and finds the compiler in a single-config build too,
  `examples/api/tables.py` names the Rust twin, and
  `.github/workflows/api-bench.yml` (new, run by hand) checks and measures
  the servers on a Linux runner.

- No libm unless a program calls into it (`src/runtime/runtime.h`
  `gs_fmod`, `src/runtime/runtime_ext.h`, `stdlib/math.goose`,
  `src/codegen_values.h`, `src/build.h`): float `%` is the runtime's exact
  remainder, a long division of the significands; math's floor, ceil,
  trunc and round are the runtime's, an integer conversion below 2^52 (the
  instruction where SSE4.1 or arm64 has one); sqrt under gcc and MSVC on
  x86-64 is SSE2's, with no errno; and Linux links libm `--as-needed`.
  Each gives libm's result to the bit (`test/stdlib/math_rounding.goose`,
  new: signed zeros, halfway cases, 2^52, subnormals, infinities and NaN,
  under clang, gcc, MSVC, TinyCC and MIR). A program then does not load
  libm.so, a third of a megabyte resident on Linux, where `compare/`'s
  baseline was a tenth of a megabyte above Rust's.

- `src/runtime/runtime_ext.h` (and `src/runtime_inline.h`, regenerated),
  `docs/stdlib.md`: std's `popcount`, `clz` and `ctz` take no branch where
  the C compiler has no instruction for them (TinyCC, MIR, and MSVC's
  `popcount` below `/arch:AVX`), where each was a loop over the bits: a
  dictionary probe built on `ctz` had `compare/`'s hashmap take 2.3x its
  time under `gust run` and 1.9x under `--mir`. `ctz` and `clz` reduce a
  word to the ones up to its lowest one bit, or smeared down from its
  highest, which Kim Walisch's de Bruijn multiply sends to a 64-byte table
  of bit indexes, plus a setcc for 64 at zero; `popcount` sums in fields of
  2, 4 and 8 bits and one multiply. Per call, over words whose lowest or
  highest one bit is anywhere, under `gust run`: `ctz` 47 to 3 ns, `clz`
  46 to 5, `popcount` 23 to 4; under `--mir`: 23 to 1.5, 24 to 2.7, 16 to
  2. Dense random words, where the loops stopped soonest, gain too.
  `test/stdlib/stdlib_bits.goose` (new) checks all three against counting
  bit by bit, for zero, each bit, runs, every pair of bits and random words.

- `format_flt` from the float's exact value (`gs_fmt_fixed`,
  `src/runtime/runtime_os.h`), half up at an exact tie: it had computed
  `x * 10^d + 0.5` in floats and an `i64`, which rounded 1.115 to 1.12 at two
  places and wrapped past 2^63, printing `format_flt(1e19, 2)` as
  38758200196842127.36. 199,889 cases agree with Python's exact decimals.
  `hash` of a float is of its bits (`gs_flt_bits`), the two zeros as one,
  where counting 1/65536ths gave every small float one hash; and `hash_mix`
  folds the high half down first, where keys differing only above bit 32
  shared a dictionary's low bits. 100,000 keys `i << 44` take 0.003 s, from
  1.75 s. `test/stdlib/format_flt_hash.goose` (new).
- A float's text and reading it back (`src/runtime/runtime_impl.h`,
  `gs_grisu3`, `gs_dec_to_f64`; `stdlib/std.goose`, `parse_flt`). Formatting
  finds the digits by Grisu3, as Rust's does, where every float took big
  integers, which stay for the 0.25% to 0.8% it cannot prove: the same text
  for every float (80 million compared with the integers' digits, none
  differing), at 0.86x Rust's cycles, from 7.1x. Reading had been off by a
  unit in the last place for up to one float's shortest text in five, which so
  did not read back; `parse_flt` now gives the nearest float, ties to even, as
  Rust's and Go's do: Clinger's exact case, else 64-bit scaling through the
  same 87 powers of ten with its uncertainty tracked, else the text compared
  exactly with the points halfway between doubles. 510,000 hard texts agree
  with Python's. `test/stdlib/float_text.goose` (new).

- `stdlib/dictionary.goose`: a slot's tag is one byte, the top seven bits
  of the key's hash, where it was the low 31 with the home slot in them, so
  growth and removal hash the keys they move again. A slot of `i64` keys and
  values takes 17 bytes, as in Rust's hashbrown, against 20: `compare/`'s
  hashmap peaks at 9.4 MB where it took 10.6 (Rust 9.1) and runs as fast.
  Growing a table of slice keys from empty costs a third more time than
  before, still less than Rust's.

- `stdlib/dictionary.goose`: a Swiss table in Go's group layout
  (`docs/gust/performance_designs.md`): groups of eight slots, each with a
  control word of one byte per slot (empty, deleted, or the hash's top seven
  bits), in the one grow-shrink array, so references into it keep §5.2's
  rules. A lookup matches a group's eight bytes at once (the SWAR zero-byte
  test) and stops at a group with an empty byte; the table fills to 7/8 and
  doubles in place, or rehashes at its own size where tombstones are most of
  what fills it (hashbrown's rehash_in_place over an array that may have
  grown); removal leaves a tombstone only in a full group. The probes hand
  back the slot they found, so no bounds check is left, and a match's slot
  is a multiply rather than std's `ctz`, which is a loop under TinyCC and
  MIR (with it `gust run` took twice the time). Measured with
  `compare/quiet_bench.py` on Windows against the one-byte-tag table:
  `compare/`'s hashmap at 0.77x its time with clang and 0.84x with MSVC,
  strings at 0.86x and 0.87x, the same peak memory; under MIR 0.53x and
  0.86x, under TinyCC level. Grown from empty to 885k integer keys it peaks
  at 22.6 MB where it took 40.4 (Rust 32.3), and 82.8 MB for string keys
  where it took 108.9 (Rust 97.1). That table is 84% full where the old one,
  just doubled, was 42%, so lookups cost more there: 1.23x the time with
  integer keys (0.77x Rust's), 1.02x with strings. Removing half and adding
  as many keys twice over leaves enough tombstones to double it, as it does
  Rust's: the same peak as before, 1.15x and 1.00x the time. On Linux under
  cachegrind (Bench runs 41257002851 before, 41259326574 after) hashmap
  takes 12% more instructions and a third of the mispredicted branches (3.0
  million, from 9.0), strings 14% more and 0.63x. Both runs before had a
  faster Linux runner than both runs after, so the times there do not
  compare; the local ones above do.
  `test/stdlib/stdlib_dictionary_model.goose` gains a case that forces a
  rehash at the table's own size; `samples/expected/serialize.out` follows
  `each`'s new order.

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

- `each_utf8` took any bytes after a lead byte as its sequence: `a\xE2\x82b` lost
  its `b`, a quote after `\xC3` went with it, and overlong encodings, surrogates
  and values past U+10FFFF came through as code points. It now passes a byte that
  starts no well-formed sequence alone, as 0xFFFD, as Go does. `push_utf8` writes
  0xFFFD for what is not a code point, so it never writes malformed text, and
  `json` decodes a lone surrogate to that rather than to its own three bytes.
  `format_int` puts the sign after padding that is not zeros (`   -42`, not
  `-   42`). `abs(-0.0)` is `0.0`. `rand_int` draws again past the last whole
  multiple of a range above 2^32, where a remainder gave 56% of its values to the
  lower half; below 2^32 it gives the sequence it always gave.
  `test/stdlib/text_edges.goose`.
- `http`'s server refuses what a proxy in front of it could read differently, the
  way a request gets smuggled inside another (RFC 9112 §11.2): a space before a
  header's colon, which left `Content-Length : 27` an unknown header and the body
  the next request; a line folded onto the one before; a bare CR or LF; control
  bytes in a method, target or header. It answers HEAD with the headers alone
  (a body went out before, which a client then read as the next response), and a
  GET route answers it, as Go's. A client waiting on `Expect: 100-continue` is told
  to send, where curl sat out a second before any body past 1 KB; another
  expectation gets a 417. An HTTP/1.0 client asking for keep-alive is told it got
  it. `serve` takes a body limit, 8 KB if not given. The client's `unchunk` no
  longer overflows on a chunk size near 2^63, which ended the program, and takes a
  reply to HEAD as having no body. `test/stdlib/http_strict_head.goose`,
  `test/threads/http_protocol.goose`.
- `to_lower` and `to_upper` work 16 bytes at a time on x86-64 in SSE2 under any
  compiler, where MSVC's and gcc's vectorizers would not take the loop: 11 times
  faster there, and level with clang.
- `dictionary` defends itself against keys chosen to collide. Whoever knows a
  hash can make every insert walk past all the keys before it: 16,000 crafted
  24-byte keys took a second, and four times as many twenty times as long. An
  insert whose probe has seen too much (a group passed counts 1, a key compared
  in vain 4, an attack at 64; ordinary keys, which stay well short of it at any
  size, never set it off in the tests) gives the table a random seed and
  rehashes it, keyed with SipHash-1-3, the hash Rust's `HashMap` uses on every
  key. Those 16,000 keys now take 2 ms. A table that is not under attack costs
  what it did, and keeps its order from run to run. `hash` of bytes and
  `hash_combine` take the key too, through `hash_flood_key`, a global, so a user
  key type built from them is covered as written.
  `test/stdlib/dictionary_flood.goose`.

## Runtime

- `src/runtime/runtime_os.h` (and `src/runtime_inline.h`, regenerated): on Windows, paths
  whose full form reaches 200 characters go to the wide APIs in their `\\?\` or `\\?\UNC\`
  form, so that `make_dir`, `read_file` and the rest work past the old 260-character limit
  (`test/stdlib/stdlib_os_longpath.goose`, new). A program's arguments come from the wide
  command line where it matches `argv`, so that a name the ANSI code page cannot spell
  reaches the program whole, built or under `gust run` (`test/os_args.py`, new). Environment
  variable names and library paths keep their old conversion. `docs/stdlib.md` says so.
- Off Windows a queue's two ends have small locks of their own, on the line of what they guard, so
  that a take moves one contended cache line and not two: a sender and three receivers took 0.51x to
  0.56x as long on x86 Linux, ahead of Rust and level with or ahead of Go, and 0.41x to 0.43x on ARM.
  A sender no longer signals a receiver already signalled, which on two CPUs had it making a system
  call per message for a time. `gs_pf_relax` is `isb` on ARM, as `yield` takes about a cycle there, and
  messages back and forth took 0.06x to 0.08x as long. With one CPU there is no spin before a sleep:
  0.03x to 0.12x as long on x86, and 0.81x on ARM. Every interleaving of the sleeping protocol was
  checked for up to three threads on each end.
- A queue's receivers look at what they last saw of the tail before they look at the tail's
  own cache line, so that with messages known to wait they leave it to the sender. On one
  machine a sender and three receivers went from 0.071 s to 0.027 s on Windows, past the
  one-lock queue's 0.058 s and with its spread gone, and took 0.93x as long on Linux.
- stdout is buffered where it is a file or a pipe, as in C, Python and Nim: 300,000 prints
  went from 0.21 s to 0.009 s, with Rust at 0.25 s and Go at 0.31 s. A print more than 50 ms
  after the last write takes what waits with it, so logs and progress lines come out as
  they did. What waits goes out before a thread blocks and before anything goes to stderr,
  at an abort or a fault, and at SIGINT, SIGTERM and SIGHUP, with a write in progress left to
  finish so that nothing is cut or comes out twice. A reader that has stopped taking bytes
  gets 2 seconds, so that `prog | less` and Ctrl-C still ends the program. What can still be
  lost is what a process killed outright had in the middle of a burst. A terminal, `GUST_UNBUFFERED` in the
  environment, foreign C in the program, a C host and a run inside the compiler all keep a
  write per print. `scripts/stdout_tests.py`.

- `thread_spawn` reuses threads. A thread whose worker has ended waits 50 ms
  for the next one, up to 64 of them, and everything of a worker but its native
  stack's pages is set up and released as on a new thread; one that aborted is
  not kept, its stack possibly having overflowed. A spawn and wait of an empty
  worker fell from 90 microseconds to about 1, near a goroutine's. A thread also
  keeps up to four of its worker's data stack regions, with no more than the
  first 64 KB of each in memory, so a worker with a data stack costs 2 microseconds
  rather than 42 (a goroutine doing the same costs 8 and a Rust thread 108). Threads
  ending still leave nothing behind 50 ms later. `test/threads/thread_reuse.goose`,
  `compare/concurrency`.
- A receiver at a typed queue looks for a message for about as long as a sleep and
  a wakeup would cost, and one that loses a race for a message waits before it looks
  again, as each receiver going for a message is another trip for the lock's cache
  line from core to core: a sender and three receivers on Linux took a million
  messages from 0.49 s to 0.16 s. Senders and receivers then got a lock and a cache
  line each, so that a message does not take one lock's line from core to core and
  back: on the same machine 0.73x to 0.83x of the time there, and 0.64x to 0.75x for
  messages back and forth (`docs/gust/performance_designs.md`).
  `test/threads/queue_stress.goose`. `scripts/tsan_threads.py`, a CI job now, runs every
  program that uses threads under ThreadSanitizer, which found a race in thread reuse
  and one in `gs_net_used`, both fixed.

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
- Typed queues and mailboxes (`src/runtime/runtime_threads.h`,
  `src/codegen_builtins.h`): messages of up to 256 bytes all of one size wait
  in a ring, where each had been a `malloc` by the sender and a `free` by the
  receiver; a get hands out a copy in thread-local storage (`gs_qfree`). A
  receiver looks at the count before it takes the lock and spins a little
  before it sleeps, and a sender signals only where one sleeps. One producer to
  three consumers ran 1,000,000 messages in 0.61 s and takes 0.11 s (Rust's
  std 0.10, Go 0.08); a round trip between two threads went from 1.3 to 0.35
  microseconds (Rust's std 1.2, Go 0.6). Order, limits and refusals are as
  before; a queue that sees two sizes falls back to nodes, in order.
- `src/runtime/runtime_net.h`, `stdlib/net.goose`: a connection costs
  fewer system calls. On Linux an accept takes `accept4` with
  `SOCK_NONBLOCK | SOCK_CLOEXEC`, as Go's and tokio's do, where three
  `fcntl` calls followed it, and `net::serve` reads a connection as it opens
  it, so that a request that came with it is served in the same turn. In
  the API benchmark's connection-per-request scenario on a hosted Linux
  runner, Gust's rate over Go's went from 0.95 to 0.98.
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
  (`net.c`, `net.h`, `net.gust`, `api.build`), now removed. Then a Rust
  twin on tokio (`rust/`, a cargo project with tokio alone: the same
  minimal HTTP, routes and JSON as `go/rawserver`, passing `test_api.py
  --go`), which `bench.py` builds and runs as `rustraw` where cargo is.
- `examples/echo/` (new): a TCP echo server on `net::serve`, with no HTTP,
  and a `--check` mode that serves itself and checks the echoes.

## Third-party code

- `third_party/tinycc` (LGPL): `tcc_set_decl_func` reports each global
  declaration as it is parsed, a function, typedef, enum constant,
  object-like macro or struct layout per line with the file it is in, for
  `gust cimport` (`libtcc.h`, `libtcc.c`, `tcc.h`, `tccgen.c`, `tccpp.c`, each
  change marked `Gust:`), a function parameter whose type is a pointer to a
  typedef of void marked as such (`V`), which TinyCC's types otherwise
  forget. Nothing else in TinyCC changed, and nothing calls the hook but
  `gust cimport`.
- `third_party/mir` (new, MIT): Vladimir Makarov's MIR at commit
  `a8ab7c31cd5f9b23b77d84c60b3d83e62d9d304c`, the subset Lobster vendors
  (its `dev/external/mir`: the IR, the generator for x86-64 and arm64, and
  c2mir), copied from Lobster's checkout. Changed, each change marked
  `Gust`: square-root instructions `MIR_FSQRT` and `MIR_DSQRT` (`mir.h`,
  `mir.c`, the interpreter, `sqrtss`/`sqrtsd` in `mir-gen-x86_64.c`,
  `fsqrt` in `mir-gen-aarch64.c`); c2mir copying a block of 6 to 64
  bytes (a small struct) with moves rather than a call of `memcpy`
  (`c2mir/c2mir.c`, `block_move`); c2mir keeping `#pragma pack` (push, pop
  and a value), which Gust lays its types out with and c2mir ignored: each
  change is kept by file and line and a struct's layout caps its members'
  and its own alignment at the packing where it is defined; and the x86-64
  generator's upper bound on an instruction's size counting the REX prefix
  a register from r8 or xmm8 up takes (`get_max_insn_size`), which a float
  comparison's ucomiss passed by a byte. Then, for Windows: the x86-64
  generator stores into each page of a frame or constant alloca larger
  than a page, top down, before sp passes it, as MSVC's __chkstk does
  (`target_make_prolog_epilog`), where the stack grows a guard page at a
  time; and c2mir rejects a call of an undeclared function rather than
  declare it `int f()`, which returned garbage for a double. Then c2mir
  copies a block of 1 to 5 bytes with moves too, rather than a loop of byte
  moves through an index, and reports each function's frame objects (a
  local's offset from fp, its size, whether it holds an array) to the
  hook `gs_c2mir_frame_object`, for `src/jit_mir.h`'s `MirPromoteFrame`:
  before generation, each 8-byte place of c2mir's frame read and written
  only whole, at one type, through addresses resolved from the definitions
  before them in their block, becomes a register; an escaping address gives
  away its object (a memcpy or memset of a known size what it covers, fp
  plus a scaled index the arrays), and one that cannot be resolved leaves
  the function's frame alone. n-body runs 3.3x off a native gcc build
  rather than 6.5x, raytrace 1.8x rather than 4.4x. Then the generator's
  SSA combining (`mir-gen.c`, `cycle_phi_p`) takes no address through a
  phi in another block: conventional SSA sets a phi's variable before the
  branch ending each predecessor, so a loop latch that also leaves the loop
  had set it to the next iteration's value on the way out, and a store
  past the loop folded through it went to the wrong place. That kept Gust
  at MIR's level 1; at level 2 the compare suite runs 1.54x off rather
  than 1.94x. On arm64, the generator's test of a memory operand
  (`mir-gen-aarch64.c`, `target_memory_ok_p`) scales a displacement and an
  index by the access's size, as its patterns do, not by the size's log:
  SSA combining had folded a packed field's displacement of 57 into an
  8-byte access, which no pattern matched (230 MIR fixtures on macOS).
  And GVN's memory availability across blocks (`mem_av_trans_func`) is
  ended by every store of a block, not only by those still available at
  its end: a store a later one had made unavailable, the two possibly
  aliasing, still wrote memory an expression from before the block read,
  which aliasing that is not transitive (alias set 0) let through. And the
  generator's address transformation (`addr_eliminable_p`,
  `collect_addr_uses`) keeps an `addr` whose pseudo a store of fewer bytes
  than it has reaches: it had made `*(char *)&x = 7` an assignment of all
  of x, at level 2 (a C reproducer: an int's low byte stored through a
  char pointer to it, read back wrong).
- `third_party/mir` is built with `__WIN32` defined on Windows
  (`CMakeLists.txt`): its built-in headers test MinGW's macro, which MSVC
  and clang do not define, and c2mir's stdarg.h declared no `va_list`.

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
  `check_doc_examples.py` compiles and runs every example `--offline`, so a
  document's import of a git package (a placeholder's) fetches nothing.
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
