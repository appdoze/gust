# Gust for APIs and backend services

What Gust had for writing an HTTP API, what was built and measured in
`examples/api/`, why it serves from a pool of event loops, how it compares
with Go under the same load, and what Gust needs next. The language
research behind the design decisions is in
[`language_research.md`](language_research.md).

Every number here was measured on one machine (Windows 11, Intel i7-11700F,
8 cores and 16 threads, 32 GB) over loopback, on 25 October 2026, with
Gust built by clang 22.1.8 at `-O2` and Go 1.26.3. Raw results are in
`examples/api/results/`.

## 1. The audit: what a backend needs, and what Gust had

| area | Gust before this work | in `examples/api/` now | gap left |
|---|---|---|---|
| sockets | nothing: the standard library has files, not sockets | `net.c` (Winsock and POSIX) behind `extern fn`s checked against `net.h` by `c_headers`; blocking and non-blocking calls; a poller (WSAPoll, poll) that owns its sockets; every pointer-and-length call wrapped so a slice gives both | a `net` module in the standard library; epoll, kqueue or IOCP behind the poller for many thousands of connections per worker |
| HTTP | none | `http.gust`: HTTP/1.1 parsed in place, keep-alive, pipelining, `Content-Length` bodies, `Host` checks (RFC 9112), `413` / `431` / `501` / `408` refusals, partial reads and writes | chunked bodies, a client, HTTP/2, TLS (terminate it in a proxy meanwhile), responses over 16 KB that a slow client does not take (closed) |
| routing | none | a plain `if` chain on the path, `405` naming the methods a path has, as Go 1.22's `ServeMux` | a router with path patterns; function values cannot escape, so a table of handlers is not possible today, but an enum of routes expanded with `const for` would be |
| JSON | `std/json`: a writer, a tree parser, and a reader that decodes straight into a program's types | `jsonx.gust`: a struct of numbers, booleans and inline strings encoded and decoded through `const for` over its fields, one line per type to opt in to encoding | the opt-in line (`const if` would remove it); optional fields; nested structs and arrays in decoding; UTF-8 validation of strings |
| validation | none | field errors gathered into one FastAPI-shaped `422`; text fields with their limit in the type (`Str64`) | declarative constraints on fields |
| errors | an abort ends the program; contained workers end only themselves | workers contained under supervisors that close their sockets and restart them (section 4); refusals as JSON | an aborted worker's request counts are lost; out-of-memory is not contained |
| concurrency | `thread_fn` workers sharing nothing, typed global queues of flat values | a pool of event-loop workers, one per CPU, by default; a thread per connection as an option | handlers in the pool must not block; read-only state must be compile-time `const` or be copied; no addressed replies |
| cancellation | none | a stop flag (an atomic in C) the loops look at every 50 ms, the thread per connection between requests and every 100 ms while waiting | a handler cannot be interrupted, and no per-request deadline reaches into it (Go's `context`) |
| timeouts | none | idle (5 s between requests), request (5 s from a request's first byte, as Go's `ReadHeaderTimeout`), send (5 s for a client that stops reading) | per-route deadlines |
| graceful shutdown | none | stop accepting, finish each request in flight, answer it `Connection: close`, close idle connections, within a 10 s grace | none for this example |
| observability | none | per-worker counts sent to `main` when a worker ends | logging, metrics, a wall clock and dates in the standard library |

## 2. Why a pool of event loops

The example first served each connection on a thread of its own, as
`net/http` serves each on a goroutine. Measured, that cost too much in two
ways, and the cause was the thread itself.

**Memory per idle connection.** `examples/api/memory_profile.py` opens
idle keep-alive connections, each after one request, and reads the
server's address space with `VirtualQueryEx`, so the kinds of memory are
told apart. Per connection, as (value at N − value at 0) / N:

| server | reserved address space | committed | resident | threads |
|---|---:|---:|---:|---:|
| Gust, thread per connection | 16 GB | 293 KB | 131 KB | 2.00 |
| Gust, worker pool | 3 KB | 2.4 KB | 1.9 KB | 0 |
| Go net/http | 40 KB | 25 KB | 24 KB | 0.01 |
| Go, same HTTP as Gust | 49 KB | 34 KB | 32 KB | 0.01 |

At 1,000 connections; the thread-per-connection figures are the same at
100, 250 and 500 (linear), Go's fall with the count (goroutines share 12
OS threads, and its allocator amortizes), and the pool's are within a few
hundred bytes. Definitions:

- *reserved*: private address space, committed or not. Gust reserves 2 GB
  per data stack up front and commits it as it grows; reserved address
  space costs no memory, but it is what caps a thread-per-connection
  server at about 2,000 connections (`gust build --stacks`).
- *committed*: private memory the system has promised, which counts
  against its commit limit (RAM plus page file) whether touched or not.
  The thread-per-connection 293 KB was 152 KB of native thread stacks (two
  threads, each holding the 64 KB stack guarantee the runtime keeps for
  reporting overflow), 64 KB of data stacks (four touched regions at the
  16 KB minimum commit), and 77 KB of per-thread runtime and system state.
  (Windows' own PrivateUsage counter, which an earlier draft quoted as
  "0.33 MB", reads about 330 KB for the same connection.)
- *resident*: the working set, pages actually in RAM, private and shared.

The pool's idle connection is a `Conn` record, a poller entry and the
system's own socket state; it holds a buffer only while a request is
partly read or a response partly sent.

**Tail latency at high concurrency.** `examples/api/switches.py` counts
context switches (system-wide, as a process's own count loses the threads
that have exited); this is the diagnosing run, before the pool existed:

| server, closed loop, mixed requests | 64 connections: req/s, p99.9 | 256 connections: req/s, p99.9 | switches per request (64, 256) | threads |
|---|---|---|---:|---:|
| Gust, thread per connection | 186k, 6.7 ms | 166k, 52.9 ms | 1.23, 1.24 | 515 at 256 |
| Go, same HTTP as Gust | 149k, 6.0 ms | 149k, 12.9 ms | 0.45, 0.40 | 17 |
| Go net/http | 85k, 7.6 ms | 86k, 33.9 ms | 1.91, 1.74 | 17 |

With a thread per connection, 256 busy connections are 256 runnable
threads on 8 CPUs, and a thread whose socket is ready waits for the OS to
run it. Go multiplexes its connections over `GOMAXPROCS` threads, whose
netpoller hands each the sockets that are ready.

**What a non-blocking `thread_wait` would have done.** Half of the
threads were supervisors, one per connection, there to close a socket if
its connection's thread aborted, since Gust has no way to learn that a
contained worker ended without a thread blocked on it. A variant without
them (connections spawned contained from `main`, as a non-blocking
`thread_wait` would allow) measured 162 KB committed and 70 KB resident
per idle connection, one thread each, but p99.9 at 256 connections of
57 ms: the supervisors cost memory, not latency, because a blocked thread
never runs. So it was not the prerequisite; fewer runnable threads were.

**The design.** `http.gust`'s `serve_loop`: one worker per CPU
(`--workers`), each a contained thread with an event loop over the
connections it accepts from the shared non-blocking listener, waiting on
a poller. Its tradeoffs, chosen knowingly:

1. *Handlers must not block.* A blocking call in a handler stalls every
   connection of its worker. The example's handlers are CPU only; a
   handler that must block (a database call) needs the thread-per-
   connection mode, kept as `--thread-per-connection`, or M:N workers
   (roadmap).
2. *Containment is per worker.* Gust contains aborts per thread (spec
   §11.2), so an abort in a handler ends its worker and that worker's
   connections, about 1/N of them; its supervisor frees the worker's
   poller, which owns their sockets and so closes them, and starts a new
   worker on the same listener. With a thread per connection it ends one
   connection.
3. *More networking code.* Partial reads keep a request's start in an
   input slot; partial writes keep the rest of a response in an output
   slot and wait for the socket; while output is pending the connection is
   not read, so responses go out in order and a client that does not read
   is not sent more. Slots come from per-worker slabs that only grow, with
   free lists.

No runtime change was needed: the pool is example code over the `net`
layer.

## 3. How the example fits Gust's models

- **A request is an arena that cannot leak.** Its head and body are views
  into a buffer (the worker's scratch buffer, or its connection's slot),
  decoded into fixed-size fields, and its response is built on the data
  stack; all of it is released when the request's scope ends. The
  lifetime rules prove that no view outlives the buffer it points into,
  and the type checker refused the first version of the loop for storing
  views into a grow-shrink slab (§5.2): the slabs are grow-only. The
  generated C of the request path calls no allocator.
- **Threads share nothing, so the release needs no coordination.** The
  catalog every connection reads is a `const`, which is static data. The
  one shared mutable value is the stop flag, an atomic in C.
- **Identity without shared state.** An order's id is its connection's
  number and its count on that connection; connection numbers are
  `worker + workers × (round << 24 + k)`, unique across workers and their
  restarts, with no counter shared between threads.

## 4. What crash isolation guarantees

| failure | pool | thread per connection | why |
|---|---|---|---|
| a runtime check fails in a handler: index out of range, failed `assert`, `abort`, division by zero, a limited array overflowing, and, under `--checked`, integer overflow or an `as` conversion changing a value | that worker ends; its connections (about 1/N) are closed by its supervisor; a new worker takes its place; other workers' connections go on | that connection ends; its supervisor closes the socket | contained workers (§11.2); `test/threads/thread_contained.goose`; `test_api.py --crash-route` |
| a data stack runs into its guard gap (a value grew past `--stack-reserve`) | the process ends | the process ends | not contained (§11.2); `test/threads/thread_contained_overflow.goose` |
| the native call stack is exhausted (deep recursion) | the process ends | the process ends | not contained (§11.2) |
| the system refuses to commit memory | the process ends ("out of memory committing a data stack") | the same | a runtime failure, not contained |
| a fault in C (a bad pointer in an `extern fn`) | the process ends | the process ends | outside Gust's checks (`--externs` lists every such function) |
| `exit(code)` | the process ends | the process ends | by definition |
| an abort on the main thread | the process ends | the process ends | `test/threads/thread_contained_main.goose` |

Two costs of containment are measured or visible: an aborted worker's
request counts never reach `main` (it reports them only when it ends
normally), and its connections' clients see their connection closed
without a response, as `net/http` closes a connection whose handler
panicked.

## 5. Benchmark

### Method

Four servers with the same routes, JSON, validation errors and limits,
each passing the same behaviour tests (`examples/api/test_api.py`):

- **Gust, worker pool**: `api.gust --workers 8`.
- **Gust, thread per connection**: `api.gust --thread-per-connection`.
- **Go, same HTTP as Gust**: `go/rawserver`, the same minimal HTTP/1.1 as
  `http.gust` with a goroutine per connection, and the same
  `encoding/json` handlers as the `net/http` twin.
- **Go net/http**: `go/server`, idiomatic `net/http` and `encoding/json`.

The first three compare languages and runtimes over equivalent HTTP; the
last compares Gust's server with Go's standard library, which does more
per request (header maps, a `Date` header, chunked bodies, a `context` per
request). The tables keep them apart.

**The load generator** (`go/loadgen`) holds keep-alive connections, each
sending a fixed rotation of requests and checking every status: `mixed`
is four item reads, the item list, three valid orders, one invalid order
(`422`) and one missing item (`404`); `health`, `item` and `order` repeat
one kind. Latency is timed with `QueryPerformanceCounter`, because Go's
clock on Windows advances only every 0.5–1 ms.

- *Closed loop* (the default): each connection sends its next request
  when the last is answered, which measures the most a server sustains.
  It can hide latency during a stall: while the server stalls, the
  connections it stalls send nothing, so the requests that would have
  queued behind the stall are never timed (coordinated omission). Closed-
  loop tails understate what clients arriving at their own pace would see.
- *Fixed rate* (`-rate`): the connections send on a schedule, 20,000
  requests per second in all, and each latency is also measured from when
  the request was due, which counts the queueing a stall causes, as wrk2
  does. That second measure includes the load generator's timer slack
  (Go's sleep on Windows), the same for every server, which is why its
  medians sit near 300 µs; the service time from the request going out is
  shown beside it.
- *Churn* (`-churn`): a new connection for every request, reset after it
  (SO_LINGER 0, so no TIME_WAIT exhausts the ports).

**It is not the bottleneck** (`examples/api/loadgen_check.py`): the same
64 connections from one load generator process and from two of 32 each,
on the same client CPUs, gave the same total (Gust pool 180k and 174k
req/s; Go with the same HTTP 138k and 140k). Its own collector ran once
or twice per run, pausing at most 1.4 ms. Neither side's CPUs are
saturated (about 70–80% each), so the ceiling at these rates is in part
the loopback path and the round trips between the processes.

**Conditions.** The server runs on CPUs 0–7 and the load generator on
8–15 (so 4 physical cores each, with their hyperthreads), with
`GOMAXPROCS=8` for every Go program. Each scenario runs five times, the
servers in rotating order, 2 s of warm-up and 10 s measured; the tables
show the median and, in parentheses, the lowest and highest of the five
(`python examples/api/bench.py --reps 5`). Memory is the server's peak
working set, sampled every 50 ms; Go's allocations come from
`runtime.MemStats` before and after each run; page faults are the
server's, per thousand requests including warm-up.

### Results

`examples/api/results/2026-10-25-pool-4way-5reps.json` holds every rep;
"Gust, thread per connection" is the design before the pool, measured in
the same run, so it is the before and "Gust, worker pool" the after.

What they show:

- **The pool fixed what it was built for.** At 256 connections, p99 fell
  from 29.3 ms (thread per connection) to 4.7 ms and p99.9 from 57.6 ms to
  9.6 ms, with throughput up from 157k to 179k requests per second and
  peak memory from 37.6 MB to 6.1 MB. With a new connection for every
  request it went from 3,051 to 7,053 per second, level with both Go
  servers, and its page faults per thousand requests from 29,610 to 1.7.
- **Against the same HTTP in Go**, the pool was ahead at every
  connection count: 136k against 122k requests per second at 16
  connections, 175k against 138k at 64, 179k against 138k at 256, with
  lower p99 and p99.9 in each, and less than half the peak memory (5.9–6.1
  MB against 15.2–28.0 MB). At a fixed 20,000 requests per second the
  three are within each other's ranges.
- **Against Go's net/http** the gap is wider (about 2.2x the throughput at
  64 connections), and most of it is the library: Go with the same minimal
  HTTP gets 75% more than Go's net/http at 64 connections.
- **Go allocates per request; Gust does not.** net/http: 30 heap
  allocations and 3.0 KB per request, collected about 2,000 times in a
  10 s run; Go with the minimal HTTP: 7 and 465 bytes (its
  `encoding/json`); Gust: none in the request path (section 7 says how
  that was established) and, in the pool, 0.06 page faults per thousand
  requests.
- **The runs vary.** Ranges over five reps are in the tables: the pool's
  throughput at 64 connections spans 172k–179k, its p99.9 4.3–4.6 ms; the
  fixed-rate tails span 3x or more for every server. The machine was not
  otherwise idle: measured afterwards, other programs kept its server CPUs
  about 27% busy, uncontrolled during the runs. One churn rep of the pool
  lost a client to a load-generator dial failure (Windows ran out of
  socket buffers after about 70,000 connections, `WSAENOBUFS`), which is
  its 5,181 low end.
- **Not better everywhere.** The pool's p95 is above the thread per
  connection's at 64 and 256 connections (886 against 716 µs, 3,075
  against 2,452 µs), and at 16 connections the thread per connection has
  the lower p99 (391 against 420 µs): the OS wakes a ready thread at once,
  while a worker answers its ready connections one after another. The
  pool's gain is in throughput and in the far tail, where the thread per
  connection's queueing for CPUs shows.

**Equivalent minimal HTTP**

| scenario | server | req/s (range) | p50 µs | p95 µs | p99 µs (range) | p99.9 µs (range) | peak RSS MB | server CPU | load gen CPU | errors |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| mixed, 16 connections | Gust, worker pool | 136,356 (122,395–145,189) | 95 | 228 | 420 (352–475) | 870 (757–1,343) | 5.9 | 69% | 72% | 0 |
| mixed, 16 connections | Gust, thread per connection | 134,366 (133,524–138,523) | 103 | 214 | 391 (368–404) | 1,153 (1,094–1,227) | 6.7 | 71% | 71% | 0 |
| mixed, 16 connections | Go, same HTTP as Gust | 121,998 (120,278–126,693) | 111 | 241 | 454 (409–507) | 1,459 (1,171–1,682) | 15.2 | 70% | 67% | 0 |
| mixed, 64 connections | Gust, worker pool | 175,353 (172,104–179,269) | 272 | 886 | 2,258 (2,189–2,290) | 4,455 (4,263–4,625) | 5.9 | 75% | 79% | 0 |
| mixed, 64 connections | Gust, thread per connection | 159,041 (152,191–163,437) | 307 | 716 | 2,988 (2,602–3,622) | 9,162 (7,748–9,679) | 12.9 | 78% | 76% | 0 |
| mixed, 64 connections | Go, same HTTP as Gust | 137,847 (126,586–142,354) | 378 | 901 | 2,972 (2,829–3,497) | 6,397 (6,144–7,387) | 15.9 | 76% | 70% | 0 |
| mixed, 256 connections | Gust, worker pool | 179,171 (159,916–179,709) | 1,188 | 3,075 | 4,677 (4,551–5,299) | 9,612 (8,487–10,366) | 6.1 | 75% | 82% | 0 |
| mixed, 256 connections | Gust, thread per connection | 156,835 (155,808–162,783) | 1,043 | 2,452 | 29,285 (23,446–32,175) | 57,568 (56,401–59,035) | 37.6 | 80% | 79% | 0 |
| mixed, 256 connections | Go, same HTTP as Gust | 138,263 (135,059–138,958) | 1,746 | 2,660 | 6,129 (5,856–6,350) | 13,958 (13,178–14,328) | 28.0 | 78% | 72% | 0 |
| health, 64 connections | Gust, worker pool | 175,115 (146,493–179,845) | 266 | 921 | 2,302 (2,071–3,068) | 4,912 (4,632–7,696) | 5.9 | 72% | 80% | 0 |
| health, 64 connections | Gust, thread per connection | 160,058 (157,968–165,008) | 318 | 716 | 2,443 (2,313–2,580) | 7,945 (7,644–9,394) | 12.9 | 77% | 77% | 0 |
| health, 64 connections | Go, same HTTP as Gust | 149,624 (147,815–152,660) | 358 | 809 | 2,424 (2,350–2,464) | 5,550 (5,305–5,802) | 15.9 | 75% | 74% | 0 |
| order, 64 connections | Gust, worker pool | 167,358 (157,880–171,708) | 278 | 941 | 2,450 (2,271–2,879) | 5,106 (4,670–6,417) | 5.9 | 71% | 76% | 0 |
| order, 64 connections | Gust, thread per connection | 153,864 (145,108–158,375) | 333 | 794 | 2,544 (1,184–2,831) | 8,161 (5,071–10,294) | 12.9 | 77% | 74% | 0 |
| order, 64 connections | Go, same HTTP as Gust | 125,927 (120,268–139,217) | 407 | 1,085 | 3,064 (2,578–3,214) | 6,915 (5,761–7,569) | 15.9 | 71% | 66% | 0 |
| mixed, new connection per request, 16 clients | Gust, worker pool | 7,053 (5,181–7,152) | 1,440 | 2,628 | 3,564 (3,380–5,262) | 16,564 (11,716–21,919) | 6.0 | 13% | 49% | 1 |
| mixed, new connection per request, 16 clients | Gust, thread per connection | 3,051 (3,022–3,162) | 4,575 | 5,761 | 7,944 (7,366–8,416) | 19,062 (14,693–25,560) | 10.4 | 50% | 22% | 0 |
| mixed, new connection per request, 16 clients | Go, same HTTP as Gust | 7,368 (7,265–7,953) | 1,585 | 2,225 | 2,796 (2,388–3,182) | 9,561 (5,593–19,559) | 14.8 | 23% | 48% | 0 |

At a fixed rate: service time (from the request going out) and, in the last three columns, the
time from when it was due (coordinated omission corrected; includes the load generator's timer slack).

| scenario | server | p50 µs | p99 µs (range) | p99.9 µs (range) | due p50 µs | due p99 µs (range) | due p99.9 µs (range) |
|---|---|---:|---:|---:|---:|---:|---:|
| mixed, 64 connections, 20k req/s fixed rate | Gust, worker pool | 86 | 302 (174–470) | 871 (296–2,582) | 334 | 1,133 (856–1,529) | 3,084 (1,407–10,186) |
| mixed, 64 connections, 20k req/s fixed rate | Gust, thread per connection | 82 | 186 (152–669) | 556 (436–3,359) | 337 | 985 (954–2,187) | 7,226 (1,662–9,577) |
| mixed, 64 connections, 20k req/s fixed rate | Go, same HTTP as Gust | 112 | 263 (259–304) | 1,032 (502–1,509) | 300 | 843 (804–858) | 5,009 (1,278–8,728) |

**Gust against Go's net/http**

| scenario | server | req/s (range) | p50 µs | p95 µs | p99 µs (range) | p99.9 µs (range) | peak RSS MB | server CPU | load gen CPU | errors |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| mixed, 16 connections | Gust, worker pool | 136,356 (122,395–145,189) | 95 | 228 | 420 (352–475) | 870 (757–1,343) | 5.9 | 69% | 72% | 0 |
| mixed, 16 connections | Go net/http | 82,960 (78,599–85,147) | 149 | 453 | 1,009 (951–1,118) | 2,098 (1,835–2,650) | 18.8 | 74% | 50% | 0 |
| mixed, 64 connections | Gust, worker pool | 175,353 (172,104–179,269) | 272 | 886 | 2,258 (2,189–2,290) | 4,455 (4,263–4,625) | 5.9 | 75% | 79% | 0 |
| mixed, 64 connections | Go net/http | 78,585 (72,359–80,077) | 487 | 3,140 | 5,218 (4,964–5,833) | 8,707 (8,230–9,551) | 20.7 | 74% | 49% | 0 |
| mixed, 256 connections | Gust, worker pool | 179,171 (159,916–179,709) | 1,188 | 3,075 | 4,677 (4,551–5,299) | 9,612 (8,487–10,366) | 6.1 | 75% | 82% | 0 |
| mixed, 256 connections | Go net/http | 81,105 (80,299–82,739) | 1,812 | 13,130 | 24,595 (22,710–25,916) | 38,198 (34,788–38,561) | 28.3 | 79% | 51% | 0 |
| health, 64 connections | Gust, worker pool | 175,115 (146,493–179,845) | 266 | 921 | 2,302 (2,071–3,068) | 4,912 (4,632–7,696) | 5.9 | 72% | 80% | 0 |
| health, 64 connections | Go net/http | 92,207 (90,404–94,492) | 482 | 2,391 | 4,262 (4,165–4,386) | 6,369 (5,978–7,002) | 19.8 | 76% | 54% | 0 |
| order, 64 connections | Gust, worker pool | 167,358 (157,880–171,708) | 278 | 941 | 2,450 (2,271–2,879) | 5,106 (4,670–6,417) | 5.9 | 71% | 76% | 0 |
| order, 64 connections | Go net/http | 72,889 (66,811–80,145) | 537 | 3,345 | 5,614 (4,883–6,433) | 9,956 (8,411–10,898) | 20.6 | 71% | 45% | 0 |
| mixed, new connection per request, 16 clients | Gust, worker pool | 7,053 (5,181–7,152) | 1,440 | 2,628 | 3,564 (3,380–5,262) | 16,564 (11,716–21,919) | 6.0 | 13% | 49% | 1 |
| mixed, new connection per request, 16 clients | Go net/http | 7,025 (5,827–7,735) | 1,632 | 2,352 | 3,697 (2,420–5,894) | 13,911 (4,930–20,317) | 17.5 | 28% | 47% | 0 |

At a fixed rate: service time (from the request going out) and, in the last three columns, the
time from when it was due (coordinated omission corrected; includes the load generator's timer slack).

| scenario | server | p50 µs | p99 µs (range) | p99.9 µs (range) | due p50 µs | due p99 µs (range) | due p99.9 µs (range) |
|---|---|---:|---:|---:|---:|---:|---:|
| mixed, 64 connections, 20k req/s fixed rate | Gust, worker pool | 86 | 302 (174–470) | 871 (296–2,582) | 334 | 1,133 (856–1,529) | 3,084 (1,407–10,186) |
| mixed, 64 connections, 20k req/s fixed rate | Go net/http | 131 | 638 (528–1,913) | 2,863 (2,023–7,698) | 306 | 1,122 (946–7,728) | 5,877 (4,813–32,522) |


| scenario | server | page faults per 1k requests | heap allocations per request | heap bytes per request | collections | collector pause, total ms |
|---|---|---:|---:|---:|---:|---:|
| mixed, 16 connections | Gust, worker pool | 0.06 | 0 (see text) | 0 | none | none |
| mixed, 16 connections | Gust, thread per connection | 0.47 | 0 (see text) | 0 | none | none |
| mixed, 16 connections | Go net/http | 67.36 | 29.9 | 2,995 | 1,445 | 541.4 |
| mixed, 16 connections | Go, same HTTP as Gust | 14.67 | 7.0 | 462 | 253 | 93.8 |
| mixed, 64 connections | Gust, worker pool | 0.06 | 0 (see text) | 0 | none | none |
| mixed, 64 connections | Gust, thread per connection | 1.54 | 0 (see text) | 0 | none | none |
| mixed, 64 connections | Go net/http | 70.66 | 30.0 | 3,012 | 2,030 | 758.2 |
| mixed, 64 connections | Go, same HTTP as Gust | 20.35 | 7.0 | 465 | 491 | 278.2 |
| mixed, 256 connections | Gust, worker pool | 0.08 | 0 (see text) | 0 | none | none |
| mixed, 256 connections | Gust, thread per connection | 6.18 | 0 (see text) | 0 | none | none |
| mixed, 256 connections | Go net/http | 62.05 | 30.0 | 3,004 | 1,053 | 441.8 |
| mixed, 256 connections | Go, same HTTP as Gust | 9.35 | 7.0 | 466 | 180 | 96.2 |
| health, 64 connections | Gust, worker pool | 0.06 | 0 (see text) | 0 | none | none |
| health, 64 connections | Gust, thread per connection | 1.51 | 0 (see text) | 0 | none | none |
| health, 64 connections | Go net/http | 63.52 | 25.1 | 2,658 | 1,661 | 582.2 |
| health, 64 connections | Go, same HTTP as Gust | 21.21 | 6.0 | 434 | 405 | 195.1 |
| order, 64 connections | Gust, worker pool | 0.06 | 0 (see text) | 0 | none | none |
| order, 64 connections | Gust, thread per connection | 1.60 | 0 (see text) | 0 | none | none |
| order, 64 connections | Go net/http | 69.42 | 38.2 | 3,435 | 1,756 | 700.1 |
| order, 64 connections | Go, same HTTP as Gust | 20.05 | 11.0 | 442 | 355 | 215.4 |
| mixed, 64 connections, 20k req/s fixed rate | Gust, worker pool | 0.52 | 0 (see text) | 0 | none | none |
| mixed, 64 connections, 20k req/s fixed rate | Gust, thread per connection | 12.21 | 0 (see text) | 0 | none | none |
| mixed, 64 connections, 20k req/s fixed rate | Go net/http | 118.00 | 30.3 | 3,009 | 453 | 28.0 |
| mixed, 64 connections, 20k req/s fixed rate | Go, same HTTP as Gust | 36.03 | 7.0 | 467 | 57 | 1.2 |
| mixed, new connection per request, 16 clients | Gust, worker pool | 1.70 | 0 (see text) | 0 | none | none |
| mixed, new connection per request, 16 clients | Gust, thread per connection | 29610.41 | 0 (see text) | 0 | none | none |
| mixed, new connection per request, 16 clients | Go net/http | 189.12 | 55.8 | 4,834 | 188 | 31.0 |
| mixed, new connection per request, 16 clients | Go, same HTTP as Gust | 550.49 | 22.8 | 18,829 | 612 | 55.7 |

| server (500 idle connections, psutil) | resident per connection | committed per connection (PrivateUsage) | threads |
|---|---:|---:|---:|
| Gust, worker pool | 2 KB | 2 KB | 19 |
| Gust, thread per connection | 131 KB | 330 KB | 1003 |
| Go net/http | 27 KB | 28 KB | 12 |
| Go, same HTTP as Gust | 34 KB | 36 KB | 12 |

**Context switches** (`examples/api/switches.py`, system-wide per request,
two later runs on a busier machine, so slower than the tables above):

| server | 64 connections | 256 connections | threads |
|---|---:|---:|---:|
| Gust, worker pool | 0.44–0.56 | 0.60–0.67 | 19 |
| Gust, thread per connection | 1.42–1.54 | 1.70–1.72 | 505–515 at 256 |
| Go, same HTTP as Gust | 1.20–1.51 | 0.83–1.07 | 16–17 |
| Go net/http | 2.68–2.75 | 2.59–2.86 | 16–17 |

On that busier machine the pool's p99.9 at 256 connections was 27 and 45
ms in two runs, against 9.6 ms in the benchmark: background load moves
these tails more than any server difference. **Under controlled CPU
contention** (`examples/api/contention.py`, two busy loops pinned to two of
the server's CPUs, alternating with none), both servers' tails rose, the
pool's less: p99.9 from 12–13 ms to 24–29 ms, against 16 ms to 31–49 ms
for Go with the same HTTP, whose throughput also fell (from 121k–136k to
104k–113k) where the pool's held (157k–167k). The worry that a pool
worker owning its connections, with no work stealing, would make the
pool's tail more fragile than Go's did not show up here.

## 6. Roadmap for backends, in order

Ranked by measured impact against effort; each names its evidence.

1. **A `net` module in the standard library**, from `examples/api/net.c`
   and the event loop in `http.gust`. Every backend needs it, and the
   loop is the measured way to serve many connections (section 2). Effort:
   moderate, mostly moving and documenting code that exists and is tested
   on Windows; the POSIX half is untested here (section 7).
2. **epoll, kqueue and IOCP behind the poller.** `poll` and `WSAPoll` scan
   every socket on every wait, O(connections per worker); fine at the
   thousands measured here, not at hundreds of thousands. Effort: small
   per platform, behind the same interface.
3. **Cancellation the runtime owns**: a stop token that `qget`,
   `thread_wait` and the `net` waits return on, and a deadline per request
   a handler can see (Go's `context`, Zig 0.16's `cancel`). Today a stop
   reaches a loop within 50 ms but cannot interrupt a handler. Effort:
   runtime and compiler (new builtins).
4. **M:N workers on the data stacks**, so handlers may block at event-loop
   cost. A worker's data lives on its data stacks, so switching workers in
   user space moves no data. This is what would retire the thread-per-
   connection mode. Effort: large (runtime research).
5. **Freeze-then-share** (ROADMAP Phase 4): data built at startup and read
   by every worker in place. Today it must be a compile-time `const` or be
   copied into each thread.
6. **Addressed mailboxes**, so a worker can ask another and get the answer
   itself; without them mutable shared state (a store, a cache, a rate
   limiter) has no place in a share-nothing server.
7. **Containable out-of-memory**: a data stack the system will not commit,
   in a contained worker, could end that worker rather than the process.
8. **A non-blocking `thread_wait`.** It would halve the thread-per-
   connection mode's memory (measured, section 2), and nothing else here
   needs it.
9. **`const if` and field attributes**, so JSON needs no opt-in line and
   validation can be declared on fields, as FastAPI does with annotations;
   **a router** from an enum of routes expanded with `const for`.
10. **The rest of HTTP**: chunked bodies, a client, TLS in a package that
    wraps a C library, HTTP/2 later; logging, metrics and dates in the
    standard library.

## 7. What is not settled

- **Measured on Windows; tested on Windows and Linux; not on macOS.** On
  Linux (WSL2, kernel 6.6, gcc 15.2) the C that gust wrote for the API
  server, the stack-growth program and the contained-overflow program was
  built with gcc and run there, and `test_api.py` passed against the pool
  (normal and `--checked`) and a thread per connection, as did
  `runtime_commit_chunks.c`; `net.c` builds with `-std=c11
  -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror`, which found a `usleep`
  (gone from POSIX since 2008) that is now `poll`. The Linux Gust compiler
  itself was not built there (no CMake in that environment), so the whole
  suite ran on Windows only. macOS, whose fault-handler commit path the
  stack change touches, was not available. CI builds and tests both on
  every push (`test/api_example.py`), and no hosted run has been made for
  this work: nothing has been pushed. Nothing was benchmarked on Linux,
  whose scheduler and loopback path are not Windows'.
- **One machine, loopback.** A network between client and server adds
  latency that hides some of these differences.
- **Gust's allocation count is established from the generated C, not
  counted.** The request path (`serve_loop`'s handlers and everything they
  call, `serve` and everything it calls) contains no call to an allocator;
  slot slabs grow on a data stack only when more connections than ever
  before have a request in flight at once. Page faults per request are
  the runtime-side check. Go's numbers are its runtime's counters.
- **The Gust server is a subset of `net/http`**: no chunked bodies, no
  `Date` header, no `context` per request, no HTTP/2. The minimal-HTTP Go
  server is the like-for-like comparison.
- **Large responses to slow clients.** A worker keeps at most 16 KB of
  unsent response per connection; a larger remainder closes the
  connection (counted as a server error). The example's responses are
  under 1 KB.
