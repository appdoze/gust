# A soundness review of Gust: the brief

ROADMAP Phase 8 asks for "a soundness review of the memory model and the
new parallel features, ideally with outside reviewers", because "safer than
Rust" has to be earned by people trying to break it. This is what a reviewer
needs to start: what Gust claims, where each claim is enforced, what already
tries to break it, the escape hatches the language has on purpose, and where
the risk is highest. A finding is a program the compiler accepts that does
something §9.1 forbids, or a runtime check (§9.3) that does not fire when it
should; how to report one is at the end.

## 1. The claims

All section numbers are `docs/goose_spec.md`'s.

| claim | rule | enforced by |
|---|---|---|
| A reference or slice never outlives the variable that owns its target, and never sees its target at a wrong type. | §9.1, the outlives rule; §9.2, roots and the depth check | the checker's store and return checks (`src/typecheck_exprs.h`, `src/typecheck_flow.h`) |
| A growing or shrinking array leaves no reference or view into it usable where it could be stale. | §5.2, §7.4, §9.2 | the checker's resize facts (`src/typecheck_flow.h`) |
| A reused pool slot is never read at another type, and a stale index into it is caught. | §9.4, §5.4 | the checker and the runtime's pool checks (`src/runtime/runtime_impl.h`) |
| Indexing out of bounds, overflow in a checked build, division by zero, a pool misuse and a data stack's overrun abort; they never corrupt memory. | §9.3 | generated checks, which bounds-check elimination removes only where it proves them dead (`src/bce.h`, §10.5); the data stacks' guard gap and fault handler (`src/runtime/runtime_impl.h`) |
| A `parallel for`'s iterations cannot race: each writes only what it owns, and what they share is frozen. | §6.5, §11.2 | the checker's parallel rules (`src/typecheck_parallel.h`), the codegen's worker split (`src/codegen_nodes.h`) |
| A worker reaches another thread's data only through copies, frozen values and the runtime's channels. | §11.2 | the checker's thread and freeze rules (`src/typecheck_calls.h`, `src/typecheck_builtins.h`); the runtime (`src/runtime/runtime_threads.h`) |
| An optimization changes no result: not the inliner, base-case inlining, tail-recursion elimination, bounds-check elimination, or lanes. | — (an implementation claim) | the suite runs every fixture at `-O0` and `-O2` and compares the output (`test/run_tests.py`); lanes are bit-exact by construction (`src/codegen_lanes.h`) |

## 2. What already tries to break them

- **The suite**: 2,689 Goose programs in `test/`, of which 1,969 are
  rejections the checker must give (`test/errors_tc/`, `test/errors/`), run
  on Windows, macOS and Linux in CI, with a Linux ASan and UBSan job over
  the compiler and the C it generates.
- **The safety probes** (`compare/safety/`): fourteen programs, the same
  mistake written in Gust and Rust (`p01_oob_read` through
  `p13_parallel_callee_global`), each of which must be rejected or abort as
  `compare/safety/expected.json` says. CI runs them with `--check`.
- **Fuzzing**: `bench/compiler_fuzz.py` mutates the fixtures through the
  checker, codegen and a C compiler (its first 200,000 programs found seven
  compiler faults, all fixed); `bench/bce_fuzz.py` generates the loop shapes
  bounds-check elimination proves things about, some made unsafe on
  purpose; `test/storage/fuzz_from_bytes.goose`, `test/stdlib/fuzz_json.goose`
  and `examples/api/fuzz_http.py` fuzz what reads untrusted input (16 million
  images and 12.8 million JSON texts under ASan and UBSan with no report).
- **Issue-driven checks**: `docs/gust/soundness_rust_164250.md` takes a
  soundness bug found in Rust and tries every channel it uses in Gust (28
  programs, all rejected, 16 kept as regression tests).
- **Stress on hosted CI**: `.github/workflows/stress.yml` and `test/stress/`
  on `stress/` branches, for faults only load shows.

## 3. The escape hatches, on purpose

None of these is a finding by itself; a way to reach one without writing it
is.

- `extern fn` and `extern "lib"` blocks (§7.10): C trusted as declared.
  `--externs` lists every one outside the standard library and
  `--deny-externs` makes each an error.
- `checkfree fn` (§7.14), a function with no bounds check left, and the
  designated unsafe-fast build that removes them wholesale (§9.3).
- `as!`, the conversion that may change the value (§6.3).
- Raw serialization (`to_bytes`, `from_bytes`) of types it is defined for.

## 4. Where to look first

In order of how new and how central each is:

1. **Root structs** (§3.4.1, `src/roots.h`, `docs/design/root_structs.md`):
   lowered into separate variables before the checker, so every rule
   applies to them through that lowering. Anything the lowering misses is
   unchecked: an owner reached in a way it does not rewrite.
2. **The lanes** (`src/codegen_lanes.h`): loops run four iterations at once.
   The filter form skips blocks of iterations it proves do nothing; its
   argument (the header comment) is the thing to attack, with programs whose
   condition depends on what the loop changes.
3. **Base-case inlining's match form** (`src/optimize_basecase.h`): leaf arms
   copied into each call site, the arguments bound once; aliasing between
   arguments, and leaf arms with effects, are where a copy could differ
   from the call.
4. **`parallel for` and freezing** (§6.5, §11.2): the ownership split of the
   iterations, and what a frozen value's views may still reach.
5. **Bounds-check elimination** (`src/bce.h`, §10.5): every check it removes
   is a proof; a wrong one is an unchecked index.
6. **Relative references** (§3.9): offsets checked at store only where an
   array can span more than the width holds.
7. **The data stacks' commit on Windows** (`gs_fault_filter`,
   `src/runtime/runtime_impl.h`, and `GS_TOUCHED`, `src/runtime/runtime.h`):
   a page is committed when first touched, from the fault handler, and the
   one corruption found so far came from there (below).
8. **The MIR backend** (`gust run --mir`, `src/jit_mir.h`, `third_party/mir`
   with the fixes in `MODIFICATIONS.md`): a miscompile there is a wrong
   program from a correct one.

## 5. Known issues

- **Fixed: on Windows a vector copy into a newly committed page could store
  zeros.** On hosted Windows CI under load, `test/threads/net_serve.goose`'s
  client buffer received 16 zero bytes in every 32 from the start of a newly
  committed chunk. The C library's memcpy had faulted in its vector loop,
  and the exception that reached the fault handler recorded the thread's AVX
  state as all zero, so resuming stored the upper halves of its registers
  as zeros. A probe in the handler showed that for the commit behind every
  corrupted byte (`test/stress/net_starved.goose` reproduces it under
  `.github/workflows/stress.yml`). The runtime now makes every bulk copy,
  move and fill onto a data stack write one byte of each page first, with
  no vector state live (`GS_TOUCHED`), and the stress runs clean.
  **Not covered:** a `simd fn`'s AVX versions and the lanes write 32 bytes
  at a time themselves, and a commit fault taken in one of those stores
  could lose the same state. Their stores go to elements that already
  exist, which are committed; a 32-byte store into a stack's fresh top from
  a `simd fn`'s AVX version is the case left.
- **Root structs: references in elements and optionals, and returned
  ones, are rejected, and one in an ordinary struct's field is reached only
  through a name whose type is written** (`docs/design/root_structs.md`).
  A limit, not a hole: no other such program compiles.

## 6. Reporting

Open an issue at https://github.com/appdoze/gust/issues with the program,
the command (`gust run file.goose`, or `gust build` with the C compiler
named), the platform and what happened against what §9 promises. A
soundness bug gets a regression test in `test/errors_tc/` (a program that
must be rejected) or beside the fixture that shows the wrong result, and a
line in the release notes (`docs/gust/stability.md`).
