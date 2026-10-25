# Memory, concurrency and interfaces elsewhere, and what Gust takes

How Go, Zig, Rust, C/C++ and Odin handle data layout, strings and
collections, allocation, ownership, collection, concurrency and FFI, with
the languages in LANGUAGES.txt (Nim, Swift, Crystal, Pony, Beef, V, C3, D,
Ada/SPARK) where they decide something differently, and for each finding
the decision it leads to in Gust. Gust's memory model, its concurrency
model and the API example (`examples/api/`) were designed against this, so
the last section checks that the three fit each other.

Each finding is marked **guarantee** (what the language's specification or
documentation promises) or **implementation** (what today's compiler or
runtime happens to do, and may change). Read in October 2026, from the
official documentation and source:

| language | version | sources |
|---|---|---|
| Go | 1.26 (go1.26.3 installed) | [memory model](https://go.dev/ref/mem) (version of June 6, 2022), [GC guide](https://go.dev/doc/gc-guide), [Go 1.26 release notes](https://go.dev/doc/go1.26), [runtime/stack.go](https://github.com/golang/go/blob/master/src/runtime/stack.go), [net/http](https://pkg.go.dev/net/http) |
| Zig | 0.16.0 | [0.16.0 release notes](https://ziglang.org/download/0.16.0/release-notes.html), [language reference](https://ziglang.org/documentation/master/) |
| Rust | 1.96.0 (installed) | [Reference: type layout](https://doc.rust-lang.org/reference/type-layout.html), [std::sync::atomic](https://doc.rust-lang.org/std/sync/atomic/index.html) |
| C, C++ | C23, C++26 | C11/C++11 memory model as carried forward; C++26 P2795R5 (erroneous behaviour) via [ACCU Overload 186](https://www.accu.org/journals/overload/33/186/dargo) |
| Odin | dev-2026-09 | [overview](https://odin-lang.org/docs/overview/) |
| Nim | 2.2.12 | [memory management](https://nim-lang.org/docs/mm.html) |
| Swift | 6.2 | [TSPL: Concurrency](https://github.com/swiftlang/swift-book/blob/main/TSPL.docc/LanguageGuide/Concurrency.md), [Swift 6.2 released](https://swift.org/blog/swift-6.2-released/) |
| Crystal | 1.22 | [concurrency guide](https://crystal-lang.org/reference/latest/guides/concurrency.html) |
| Pony | tutorial, current | [reference capabilities](https://tutorial.ponylang.io/reference-capabilities/reference-capabilities.html) |
| Beef | current docs | [memory](https://www.beeflang.org/docs/language-guide/memory/) |
| V | current docs | [memory management](https://docs.vlang.io/memory-management.html) |
| C3 | current docs (no version shown) | [memory](https://c3-lang.org/language-common/memory/) |
| D | current spec | [garbage collection](https://dlang.org/spec/garbage.html) |
| SPARK | User's Guide 28.0w | [access types](https://docs.adacore.com/spark2014-docs/html/ug/en/source/access.html) |
| Hare | 0.26.0 (2026-02-13) | [introduction tutorial](https://harelang.org/tutorials/introduction/), [FAQ](https://harelang.org/documentation/faq.html), [0.26.0 release notes](https://harelang.org/blog/2026-02-13-hare-0.26.0-released/); the [specification](https://harelang.org/specification) is a draft, published as a PDF this research could not read, so nothing below rests on it |

## 1. Data layout and alignment

**Findings.**
- **Rust** (guarantee): the default representation promises only that each
  field's offset is a multiple of its alignment; fields "may be reordered".
  `repr(C)` gives declaration order with C padding; `repr(packed)` removes
  padding, and a reference to a misaligned packed field is undefined
  behaviour, so code copies the value out instead.
- **Zig** (guarantee): a plain `struct` has no guaranteed field order or
  size, only ABI-aligned fields; `extern struct` has the C ABI's layout;
  `packed struct` has exactly its backing integer's layout. The language
  recommends the plain struct unless layout matters, to leave the compiler
  free.
- **C** (guarantee and ABI): declaration order is guaranteed, padding is
  implementation-defined and fixed by the platform ABI.
- **Odin** (guarantee): `#packed`, `#raw_union`, `#align(N)`, and `#soa`,
  which stores an array of structs as one array per field with ordinary
  indexing syntax.
- **Go** (guarantee): declaration order, each field aligned
  (`unsafe.Alignof`); padding beyond that is the implementation's.
- **Hare 0.26** (guarantee): explicit padding with unnamed `_` fields, added
  because placing fields with `@offset` was, in the release notes' words,
  "quite labor intensive and cumbersome to use".

**Gust's decisions.**
- *Keep Goose's packed layout as Gust's own* (no implicit padding; padding
  is written as `pad n`). Flat values are what crosses a thread queue, and
  a packed value is the smallest copy. Unlike Rust's `repr(packed)`, a
  reference to a misaligned field is not an error in Gust: the generated
  C declares structs under `#pragma pack(1)` and makes every load and
  store an unaligned one (implementation). The spec assumes unaligned
  access is cheap (§3.2), which holds for ordinary loads on x86-64 and
  ARMv8; this research did not measure it.
- *Keep padding explicit (`pad n`)*, which Hare arrived at too, after
  offsets proved cumbersome.
- *Make the C boundary explicit, Zig's `extern struct` idea without a
  second kind of struct* (**implemented**, Part 2): `--externs` reports
  every struct an `extern fn` passes whose Gust and C layouts differ, and
  the `pad` that fixes it (`src/gust_ffi.h`). The next step is a
  `#c_layout` attribute that inserts those pads itself, so a struct shared
  with C cannot drift (roadmap, below).
- *Odin's `#soa`* is the model for the planned lane pass (ROADMAP Phase 5):
  the data-parallel loops Gust's benchmarks spend their time in want one
  array per field.

## 2. Strings and collections

**Findings.**
- **Go** (guarantee): a `string` is immutable bytes (not necessarily
  UTF-8); a slice is pointer, length and capacity over a collected backing
  array, and `append` may reallocate it, which another slice of the old
  array does not see.
- **Rust** (guarantee): `str` is always valid UTF-8; `String` and `Vec` own
  heap storage; `&str` and `&[T]` borrow it.
- **Zig** (guarantee): no string type, `[]const u8` slices with bounds
  checks, sentinel-terminated slices (`[:0]u8`) for C. In 0.16 the standard
  containers are "unmanaged": they take the allocator on every call that
  grows them.
- **Odin** (guarantee): `string` is pointer and length, UTF-8 by
  convention; dynamic arrays remember the allocator they were made with,
  and `[dynamic; N]T` has a fixed capacity and "never allocates".
- **Swift 6.2**: `InlineArray`, a fixed-size array stored inline (on the
  stack or inside another value), and `Span`, a checked view of
  contiguous memory.

**Gust's decisions.**
- *Inline bounded strings for request and record fields*, as Odin's
  fixed-capacity arrays and Swift's `InlineArray` (**used**, API example):
  `type Str64 = u8[..64]` is a 64-byte field with its length, so a decoded
  JSON string needs no allocation and its limit is part of its type. A
  string longer than the field is a validation error (`422`), not a
  truncation.
- *Slices are views with bounds checks, as in Zig and Rust*, and a
  request's method, path and body are slices of the connection's buffer
  (`examples/api/http.gust`): parsing copies nothing. Gust's lifetime rules
  prove the views die before the buffer is reused, which Zig leaves to the
  programmer and Go to the collector.
- *Bytes, not checked UTF-8*, as Go's `string`: the JSON reader validates
  escapes but not UTF-8 sequences. A validating `json::read_string` is on
  the roadmap; making every string UTF-8, Rust's way, would put a check on
  every byte from a socket.

## 3. Allocation, stack and heap, ownership

**Findings.**
- **Go**: escape analysis puts a value on the goroutine stack when the
  compiler can bound its lifetime, otherwise on the collected heap; the GC
  guide calls the analysis compiler-version dependent and says to inspect
  `-gcflags=-m` rather than rely on rules (implementation). Go 1.26 stack-
  allocates slice backing stores "in more situations".
- **Rust** (guarantee): ownership, moves and `Drop`; heap allocation is
  explicit (`Box`, `Vec`). Drop check is where rust-lang/rust#164250 lived
  (`docs/gust/soundness_rust_164250.md`).
- **Zig** (guarantee): "no default allocator"; code that allocates takes
  an `Allocator`, and a library should let its caller choose. 0.16 makes
  `ArenaAllocator` thread-safe and lock-free and calls a thread-safe
  wrapper allocator "an anti-pattern".
- **Odin** (guarantee): an implicit `context` passes `context.allocator`
  (the heap) and `context.temp_allocator`, a growing arena that the program
  clears with `free_all` once per frame or cycle.
- **C3** (guarantee): a temporary allocator scoped by `@pool`, whose data
  "you should never pass ... to other threads"; heap allocation through an
  allocator argument.
- **Beef** (guarantee): `scope` allocates on the stack until the end of the
  block (`scope::` the method, `scope:Name` a labelled block); manual
  `new`/`delete` otherwise, with a debug allocator that checks leaks.
- **Hare** (guarantee): manual memory management, `alloc` and `free` with
  `defer` beside them, and the compiler does not prevent "double-free or
  use-after-free bugs". Allocation failure is a value: `alloc`, `append` and
  `insert` can return `nomem`, which the caller must handle. Slice and array
  accesses are bounds checked; `*[*]T` opts out.
- **SPARK** (guarantee, proved): pointers follow an ownership policy —
  assignment moves, anonymous access types borrow or observe — and
  GNATprove proves the absence of leaks at assignments, scope ends and
  deallocations.

**Gust's decisions.**
- *Scope-exit release on data stacks is Gust's arena*, the guarantee Beef's
  `scope` and C3's `@pool` give, but enforced: a reference cannot outlive
  its root (the store rule), so nothing can keep a released byte, which
  C3 and Odin leave to discipline. A request's scope is its arena, cleared
  when the scope ends with no `free_all` to forget. (**Measured**, API
  example: no heap allocation per request; see
  `docs/gust/backend_api.md`.)
- *Commit data stacks in chunks that grow with the stack* (**implemented**
  in `src/runtime/runtime_impl.h`, an implementation choice, not language
  semantics). Go's goroutine stacks start at 2 KB (`stackMin = 2048`) and
  grow by copying, doubling, which Go can do because its collector knows
  every pointer into a stack. Gust's references point into data stacks, so
  they cannot move: they are reserved once and committed on fault. Goose
  committed 1 MB per fault, so every touched stack cost a megabyte of
  commit charge. Gust now commits as much as the stack already holds, from
  16 KB (the largest page size) to 1 MB. Measured on the API example with a
  thread per connection, in the process's commit charge per idle
  connection (Windows PrivateUsage): 4.1 MB before, 0.33 MB after. Of what
  remains, the data stacks are 64 KB; the rest is two native threads
  (`docs/gust/backend_api.md` breaks it down). The pool needs neither
  thread per connection: 2.4 KB.
- *Allocation failure stays fatal, so limits go in the types.* In Gust,
  memory grows implicitly (a push, a resize), so it cannot return Hare's
  `nomem` at every step; a data stack the system will not commit ends the
  process. A server bounds what a request can use instead: inline strings
  with their limit in the type, a head and a body with fixed caps, a fixed
  slot per partial request (the API example). A gap remains: an
  out-of-memory in a contained worker still ends the whole process (spec
  §11.2); ending only that worker would make it containable (roadmap).
- *An explicit heap for state that outlives a request is the gap*, where
  Go collects, Rust owns and Zig passes an allocator. Gust's long-lived
  state lives in `main` or a worker's outermost scope. Read-only state
  shared between threads has to be a compile-time `const` (static data) or
  copied into each worker; see "freeze, then share" below.

## 4. Collection

**Findings.**
- **Go** (implementation): a concurrent, non-moving mark-sweep collector;
  Green Tea, the default in 1.26, is expected to cut collection overhead
  10–40%. `GOGC` trades memory for collector CPU; `GOMEMLIMIT` is a soft
  limit.
- **D** (implementation): a conservative, stop-the-world collector by
  default; `@nogc` makes any collector allocation a compile error.
- **Nim 2.2** (guarantee): ORC by default, reference counting with a cycle
  collector; ARC, the same without cycles, frees "when it goes out of
  scope". Under ORC, "whole subgraphs are moved between threads" rather
  than counted atomically.
- **V** (implementation): a tracing collector by default; `-autofree` is
  "still WIP" by its own documentation.

**Gust's decision.** *No collector, and no reference counting.* Gust's
values are freed at scope exit, so there is nothing to collect and no
count to maintain; the benchmark measures what that buys a server's tail
latency against Go's collector (`docs/gust/backend_api.md`, with Go's
collections counted from `runtime.MemStats`).

## 5. Concurrency memory models and atomics

**Findings.**
- **Go** (guarantee): data-race-free programs are sequentially consistent;
  `sync/atomic` operations behave "as though executed in some sequentially
  consistent order". A race is not undefined behaviour in general: a
  word-sized read sees some write, never one "out of thin air". But a race
  on a multi-word value (a slice, string, interface or map) "can in turn
  lead to arbitrary memory corruption".
- **Rust, C, C++** (guarantee): the C++20 model; "data races are undefined
  behavior"; orderings from `Relaxed` to `SeqCst`; overlapping atomic
  accesses of different sizes are not allowed.
- **Zig** (guarantee for the operations, implementation for the model):
  atomic builtins take explicit orderings; the language reference
  describes no memory model of its own.
- **Swift 6.2** (guarantee): actors let "only one task access their
  mutable state at a time"; `Sendable` marks what may cross between
  concurrency domains, and "most data races produce a compile-time error".
- **Pony** (guarantee): reference capabilities; `iso` (isolated), `val`
  (immutable) and `tag` (identity) cross between actors, `ref` cannot. "By
  sharing only immutable data and exchanging only isolated data we can
  have safe concurrent programs without locks."
- **Crystal 1.22** (guarantee): fibers and CSP channels, one fiber at a
  time by default, parallelism opt-in.

**Gust's decisions.**
- *Keep share-nothing threads with flat-value queues.* It is a stronger
  guarantee than any above: there is no shared mutable memory to race on,
  so Gust needs no memory model for its own code and no atomics in the
  language. It is Pony's `iso` (a message is a copy no one else holds),
  with no capabilities to learn because nothing can be shared.
- *The C side keeps C's model.* The one shared mutable value in the API
  example is the shutdown flag, set by a signal or console handler and read
  by every connection thread. It is in C (`examples/api/net.c`), and it is
  an atomic with release and acquire, not `volatile sig_atomic_t`: that is
  enough between a signal handler and its own thread, but between threads
  C11 makes it a data race (**implemented**). The `--externs` audit lists
  every such function (Part 2).
- *What share-nothing costs* (gaps, roadmap): sharing read-only data copies
  it, and replies have no address. Pony and Swift both have an answer that
  keeps their guarantee:
  - **Freeze, then share** (Pony's `val`, Swift's immutable `Sendable`): a
    value built at startup, frozen, then read by every worker without a
    copy. Gust's `const` globals are this already, but only for
    compile-time data. The API example's catalog is a `const` for that
    reason; a catalog loaded from a file would be copied into every
    connection thread.
  - **Addressed mailboxes** (Pony's actors, Erlang's processes): Gust's
    queues are one per type, global, so a worker cannot ask another for an
    answer and get it back itself. That is why the API example gives
    orders ids no thread coordinates (the connection's number and its count
    on that connection) instead of asking a store thread for one.

## 6. Scheduling

**Findings.**
- **Go** (implementation): M:N goroutines on `GOMAXPROCS` threads; a
  goroutine starts with the average stack size seen at the last
  collection, at least 2 KB; `net/http` serves each connection on "a new
  service goroutine".
- **Zig 0.16** (guarantee for the interface): all I/O takes an `Io`;
  `Io.Threaded` is "feature-complete and well-tested", `Io.Evented`
  (stack switching with work stealing, M:N) is experimental; `io.async`
  returns a `Future`, and `cancel` "requests the `Io` implementation to
  interrupt the operation and return `error.Canceled`".
- **Crystal** (implementation): each fiber reserves an 8 MB stack and
  starts with 4 KB of it.
- **Swift** (guarantee): structured tasks on a cooperative pool;
  cancellation is cooperative (`Task.checkCancellation`).
- **Hare** (design, its FAQ): no threads in the standard library ("Probably
  not"); event loops for I/O, and processes with shared memory for
  parallel work, since "the upstream standard library does not make
  reentrancy guarantees".

**Gust's decisions.**
- *An event loop per worker for I/O, a worker per CPU* (**implemented** in
  the API example, `http.gust`'s `serve_loop`). Gust runs one OS thread per
  `thread_fn`, and the example first gave each connection one, as
  `net/http` gives each a goroutine. Measured, that cost 2 threads, 293 KB
  of commit and 131 KB resident per idle connection, and, past the CPU
  count, a tail the OS scheduler sets: 256 runnable threads on 8 CPUs put
  p99.9 at about 50 ms, against 13 ms for Go with the same HTTP. A pool of
  event loops keeps the runnable threads at the CPU count, which is what
  Go's netpoller and Hare's advice do too; measured at 256 connections,
  its p99.9 was 9.6 ms (thread per connection 57.6 ms, Go with the same
  HTTP 14.0 ms), at about 2 KB per idle connection. The handler code stays
  straight-line, with no async colouring, but must not block; the thread
  per connection stays as an option for handlers that do
  (`docs/gust/backend_api.md`).
- *Cancellation is cooperative, as in Swift and Zig.* The example's
  connections look at the stop flag between requests and while idle, and
  finish the request they are on. Zig's `cancel` (interrupt the blocked
  operation) is the next step: a runtime-owned stop flag that blocking
  queue and socket waits also return on.
- *M:N later, through the data stacks* (roadmap). The event loop gives up
  blocking handlers. A Gust worker's data lives on its data stacks, not its
  native stack, so a worker could run on a small native stack and be
  switched in user space (Zig's `Io.Evented`, Crystal's fibers) without
  moving any data: blocking handlers at event-loop cost.

## 7. FFI

**Findings.**
- **Go** (implementation): a cgo call leaves Go code for C's calling
  convention and stack, at a cost per call; Go 1.26 cut that baseline
  overhead by about 30%.
- **Zig** (guarantee): `extern` functions follow the C ABI, "for passing
  structs and unions by value" too; Zig translates C headers into Zig
  declarations (translate-c), so a header and its use cannot disagree.
- **Odin** (guarantee): `foreign import` blocks default to the C calling
  convention; Odin's own convention passes the `context` pointer as a
  hidden argument.
- **Rust** (guarantee): `extern "C"` and `repr(C)`; calling one is
  `unsafe`, and the idiom is a safe wrapper that upholds the foreign
  function's contract.

**Gust's decisions.**
- *Let the C compiler check prototypes against the C headers*
  (**implemented**, Part 2): a sidecar's `c_headers` puts the header ahead
  of Gust's own prototypes, the check Zig's translate-c gives, without
  translating C. The API example's `net.h` is checked this way.
- *Rust's safe-wrapper idiom, as the documented pattern*: the example's
  `recv_into` and `send_all` are the only callers of the pointer-and-length
  externs, and derive both from one slice
  (`examples/api/net.gust`); `--deny-externs` keeps the rest of a program
  from calling C directly.
- *No hidden argument across the boundary*, unlike Odin's context: Gust's
  stacks reach C only as explicit pointers.

## 8. Errors and failure in a server

**Findings.**
- **Go** (implementation, net/http): "any panic from ServeHTTP aborts the
  response to the client"; the server recovers it, logs it and closes the
  connection.
- **Zig** (guarantee): error unions with `try`, and `errdefer`, which runs
  "if and only if the function returned with an error from the block".
- **Swift** (guarantee): `throws`, and actor isolation for state.
- **Hare** (guarantee): errors are tagged unions; a type declared with `!`
  is an error type, `?` returns an error to the caller, `!` asserts there is
  none, and handling every case is "mandatory". 0.26 adds `_ =` to ignore
  one without asserting.

**Gust's decisions.**
- *Contain a handler's abort to its worker*, as `net/http` contains a panic
  to its connection (**implemented**, API example). Gust contains aborts
  per thread, so the granularity follows the threading: with a thread per
  connection, an abort ends one connection, and its supervisor closes the
  socket; in the pool, it ends one worker and that worker's connections,
  about 1/N of them, and the worker's supervisor frees its poller, which
  closes their sockets, and starts another. Not contained, in either: a
  data stack overrunning its reservation, native stack exhaustion, `exit`,
  a fault in C (spec §11.2; `test/threads/thread_contained_overflow.goose`
  pins the first).
- *A non-blocking `thread_wait` is not what the pool needs.* With a thread
  per connection it would remove the supervisor threads, halving memory
  (measured on a variant without them: 162 against 293 KB of commit per
  connection), but not the tail, since a supervisor never runs. The pool
  has one supervisor per worker, so it does not need one at all.
- *Validation errors are values*, accumulated into one FastAPI-shaped
  `422` response (`examples/api/jsonx.gust`), not exceptions.

## Do the three models fit together?

The API example is the test, and it holds:

- **Memory and concurrency.** Because threads share nothing, a scope's data
  stack can be released at scope exit without asking whether another
  thread still reads it. Flat values copied through queues are the only
  things that cross, and the packed layout keeps those copies small. Each
  model is what makes the other cheap.
- **Memory and the API.** A request is parsed into views of its
  connection's buffer, decoded into inline-string fields and answered from
  a body built on the data stack. Every byte of it is released when the
  request's scope ends, with no allocator in the handler's code at all.
- **Concurrency and the API.** A handler is straight-line code, as a Go
  handler is; in the pool it must not block, with a thread per connection
  it may. Containment gives the failure isolation a server needs, per
  worker. Identity (order ids) comes from data the connection already has,
  not from shared state.

Where they pull apart, each gap is the same one seen from three sides:
**long-lived data shared between threads**. Read-only data has to be
compile-time `const` or be copied; mutable shared state has to live in one
thread that no other can address. Freeze-then-share and addressed
mailboxes close it, in that order, without giving up the share-nothing
guarantee; they lead the roadmap in `docs/gust/backend_api.md`.

## Decision log

| finding | from | Gust decision | status |
|---|---|---|---|
| `extern struct` keeps C layout apart from native layout | Zig | report layout differences at the C boundary (`--externs`) | implemented |
| the same, made automatic | Zig | `#c_layout` inserts the pads | roadmap |
| headers checked against declarations | Zig translate-c | `c_headers` in the sidecar | implemented |
| safe wrapper over the foreign contract | Rust | `recv_into` / `send_all` pattern, `--deny-externs` | implemented (example) |
| fixed-capacity arrays never allocate | Odin, Swift `InlineArray` | `Str64` inline strings in request models | implemented (example) |
| per-scope arenas (`scope`, `@pool`, temp allocator) | Beef, C3, Odin | the request scope is the arena, enforced by lifetimes | measured |
| growable stacks start small | Go (2 KB) | commit data stacks from 16 KB, doubling to 1 MB | implemented, measured |
| volatile is not synchronization | C11/C++ model | the stop flag is an atomic | implemented |
| panic contained per connection | Go net/http | contained worker plus supervisor: per worker in the pool, per connection with a thread each | implemented |
| event loops for I/O, threads for parallelism | Go netpoller, Hare FAQ, Zig `Io.Evented` | a pool of event-loop workers, one per CPU | implemented (example), measured |
| allocation failure as a value (`nomem`) | Hare | not taken; limits in the types instead; containable out-of-memory in a worker | roadmap |
| explicit padding fields | Hare 0.26 | keep `pad n` | kept |
| `val` / immutable `Sendable` shared without copies | Pony, Swift | freeze-then-share | roadmap |
| actors with addresses | Pony, Erlang | addressed mailboxes | roadmap |
| `cancel` interrupts a blocked operation | Zig 0.16 | runtime stop flag that waits return on | roadmap |
| M:N with stack switching | Go, Zig `Io.Evented`, Crystal | user-space switching of workers whose data is on data stacks | roadmap |
| `#soa` | Odin | the lane pass | roadmap (ROADMAP Phase 5) |
| always-UTF-8 strings | Rust | not taken: validate at the boundary instead | rejected |
| a collector or reference counting | Go, D, V, Nim | not taken | rejected |
