# Gust for APIs and backend services

What Gust had for writing an HTTP API, what was built and measured in
`examples/api/`, why it serves from a pool of event loops, how it compares
with Go under the same load, the standard library's networking module the
example now stands on (section 6), and what Gust needs next. The language
research behind the design decisions is in
[`language_research.md`](language_research.md).

Every number here was measured on one machine (Windows 11, Intel i7-11700F,
8 cores and 16 threads, 32 GB) over loopback, with Gust built by clang
22.1.8 at `-O2` and Go 1.26.3: sections 2 to 5 on 25 October 2026, with
the example's first networking layer (its own `net.c`), and section 6's
before-and-after comparison on 26 October 2026. Raw results are in
`examples/api/results/`.

## 1. The audit: what a backend needs, and what Gust had

| area | Gust before this work | in `examples/api/` now | gap left |
|---|---|---|---|
| sockets | nothing: the standard library has files, not sockets | the standard library's `net` (section 6): TCP over IPv4, non-blocking sockets, a poller (WSAPoll, poll), checked handles, and `serve`, the event loop, over `src/runtime/runtime_net.h`; no pointer crosses into C | IPv6, DNS, UDP, TLS; epoll, kqueue or IOCP behind the poller for many thousands of connections per worker |
| HTTP | none | `stdlib/http.goose`, a protocol over `net::serve`: HTTP/1.1 parsed in place, keep-alive, pipelining, `Content-Length` bodies, `Host` checks (RFC 9112), `413` / `431` / `501` / `408` refusals, partial reads and writes, responses built whole up to 1 MB and streamed in chunks past that | chunked request bodies, a client, HTTP/2, TLS (terminate it in a proxy meanwhile) |
| routing | none | a plain `if` chain on the path, `405` naming the methods a path has, as Go 1.22's `ServeMux` | a router with path patterns; function values cannot escape, so a table of handlers is not possible today, but an enum of routes expanded with `const for` would be |
| JSON | `std/json`: a writer, a tree parser, and a reader that decodes straight into a program's types | `jsonx.gust`: a struct of numbers, booleans and inline strings encoded and decoded through `const for` over its fields, one line per type to opt in to encoding | the opt-in line (`const if` would remove it); optional fields; nested structs and arrays in decoding; UTF-8 validation of strings |
| validation | none | field errors gathered into one FastAPI-shaped `422`; text fields with their limit in the type (`Str64`) | declarative constraints on fields |
| errors | an abort ends the program; contained workers end only themselves | workers contained under supervisors that restart them, the runtime closing the sockets an ended worker held (section 4); refusals as JSON | an aborted worker's request counts are lost; out-of-memory is not contained |
| concurrency | `thread_fn` workers sharing nothing, typed global queues of flat values | a pool of event-loop workers, one per CPU, by default; a thread per connection as an option | handlers in the pool must not block; read-only state must be compile-time `const` or be copied; no addressed replies |
| cancellation | none | a stop flag (an atomic in the runtime, `net::stop_requested`) the loops look at every 50 ms, the acceptor every 100 ms | a handler cannot be interrupted, and no per-request deadline reaches into it (Go's `context`) |
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

**The design.** One worker per CPU
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
   connections, about 1/N of them; the runtime closes the worker's poller
   and its sockets as the worker's thread program ends, and the supervisor
   starts a new worker on the same listener. With a thread per connection
   it ends one connection.
3. *More networking code.* Partial reads keep a request's start in an
   input slot; partial writes keep the rest of a response queued and wait
   for the socket; while output is pending the connection is not read, so
   responses go out in order and a client that does not read is not sent
   more. That loop was first the example's own (`serve_loop`); it is now
   the standard library's `net::serve` (section 6), and `stdlib/http.goose` is
   only the protocol.

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
  shared mutable values are the stop flag, an atomic, and the runtime's
  tables of listeners and detached connections, under one lock.
- **Identity without shared state.** An order's id is its connection's
  number and its count on that connection; connection numbers are
  `worker + workers × (round << 24 + k)`, unique across workers and their
  restarts, with no counter shared between threads.

## 4. What crash isolation guarantees

| failure | pool | thread per connection | why |
|---|---|---|---|
| a runtime check fails in a handler: index out of range, failed `assert`, `abort`, division by zero, a limited array overflowing, and, under `--checked`, integer overflow or an `as` conversion changing a value | that worker ends; the runtime closes its connections (about 1/N) as it ends; its supervisor starts a new worker; other workers' connections go on | that connection ends; the runtime closes its socket | contained workers (§11.2); `test/threads/thread_contained.goose`; `test/threads/net_serve.goose`; `test_api.py --crash-route` |
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
  `stdlib/http.goose` with a goroutine per connection, and the same
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
  `encoding/json`); Gust: none in the request path (section 8 says how
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

**Against Rust on tokio** (27 October 2026, `bench.py --quick --only "mixed,
64 connections" --kinds gust,goraw,rustraw`: one rep of 3 s on the same CPU
split, with others' programs running, so indicative only;
`examples/api/results/2026-10-27-tokio-quick.json`). `examples/api/rust/` is
the same minimal HTTP/1.1, routes, limits and JSON as `go/rawserver`, a task
per connection on tokio 1.54's multi-thread runtime with eight workers
(`TOKIO_WORKER_THREADS=8`, as the Go servers get `GOMAXPROCS=8`), and it
passes `test_api.py --go`.

| server, mixed, 64 connections | req/s | p50 | p99 | p99.9 | peak RSS | server CPU |
|---|---:|---:|---:|---:|---:|---:|
| Gust, worker pool | 168k | 303 µs | 2.2 ms | 4.4 ms | 6.4 MB | 70% |
| Go, same HTTP as Gust | 140k | 380 µs | 2.8 ms | 6.0 ms | 19.0 MB | 77% |
| Rust, tokio, same HTTP | 88k | 692 µs | 1.6 ms | 2.0 ms | 7.3 MB | 51% |

At 20,000 requests per second, fixed rate, all three keep up: p99 161 µs
(Gust), 243 µs (Go), 240 µs (tokio). Tokio's server is not short of CPU
here (51%): taking out the 100 ms read deadline it keeps per read, as Go's
twin does, changed nothing (90k). A likely limit is its runtime's one I/O
driver, which one worker polls at a time, on Windows here; a run on Linux
is still to do. The tails are tokio's best of the three, and its
throughput the lowest.

## 6. The networking module

The example's sockets and its event loop moved into the standard library
as `net` (`stdlib/net.goose`, over `src/runtime/runtime_net.h`; reference
in [`docs/stdlib.md`](../stdlib.md#net)). The example keeps only HTTP
(`stdlib/http.goose`, a protocol over the loop) and its routes, and
`examples/echo/` is a second user of the loop with no HTTP in it.

### The interface

Listeners, pollers, connections and detached connections; `listen`,
`accept`, `connect` with a timeout, non-blocking `recv` and `send`,
`want` (what a poller waits for on a connection: read, write, both or
neither), `wait` with a timeout, `shutdown` (half-close) and `close`;
`detach` and `adopt` to hand a connection to another thread; a stop flag
set by Ctrl-C, SIGINT or SIGTERM once `catch_signals` is called. Results
are counts, or one of eight negative codes; `os_error()` has the OS's
code for the last failure on the calling thread.

- **Handle validity.** Every handle carries its kind, its table entry's
  generation and, for a poller and its connections, the owning thread. A
  handle that does not name a live object of its kind (closed, its entry
  reused, from another thread, made up) gets `BAD_HANDLE` and reaches
  nothing; no call takes a pointer, only slices whose lengths the runtime
  reads. Tested in `test/stdlib/stdlib_net.goose` (closed, closed twice,
  another generation, the wrong kind, a closed poller's connections, a
  detached connection adopted twice, a connection of another poller) and
  `test/threads/net_serve.goose` (another thread's poller and connection).
  The limits of the encoding: a generation has 20 bits and a thread number
  24 (the thread's id modulo 2^24), so a stale handle would pass again only
  after its entry had been reused a million times, or, across threads,
  after 16.7 million thread programs had started.
- **Ownership and cleanup.** A poller owns its connections. Pollers and
  connections live in their thread's own table, reachable from no other.
  Listeners and detached connections are in shared tables under one lock;
  an accept or a watching poller holds a reference to its listener, so
  closing it cannot let its socket number be reused under them. When a
  thread program ends, by returning or by a contained abort, the runtime
  closes what it still owns (a hook `runtime_threads.h` runs as each
  thread program ends, installed when the network is first used), so a
  supervisor restarting an aborted worker leaks no socket. Tested in
  `net_serve.goose` (a worker that returns and one that aborts, each
  leaving a listener and a connection open: the client sees the
  connection closed, and the port refuses), and in `test_api.py`, which
  checks that the server's handle count returns to what it was once the
  connections it made have ended (64 before and 64 after, on Windows).
- **Errors.** `ERROR` (the OS failed; the connection is no good), `AGAIN`,
  `BAD_HANDLE`, `LIMIT` (a table or memory ran out), `ADDRESS` (not an
  IPv4 address or `localhost`, or the port is taken), `TIMEOUT`,
  `UNAVAILABLE` (Winsock could not be started) and `INVALID`.
- **The platform boundary.** On Windows, `ws2_32.dll` is loaded from the
  system directory the first time the network is used, so a program that
  never uses it neither links nor loads it; on clang and gcc each function
  pointer is checked against `winsock2.h`'s prototype at compile time
  (`_Static_assert` on `__builtin_types_compatible_p`); MSVC builds do not
  have that check. POSIX sockets are made non-blocking and close-on-exec,
  with `MSG_NOSIGNAL` (Linux) or `SO_NOSIGPIPE` (macOS, BSD) so a peer's
  reset is an error, not a signal. The runtime's externs are declared in
  `runtime_ext.h` and checked against their definitions by the C compiler.

### The loop: order, backpressure, limits

`net::serve(listener, poller, first, proto, limits, replies, first_id,
id_step, D, P)` runs one worker. `D` gets each connection's unconsumed
input and appends replies; `P` produces a response of any size in pieces,
called whenever none of the connection's output is waiting. `D` is not
called while output is waiting or `P` is producing, so responses go out in
request order, and a connection whose client does not read is not read
either, which blocks the client's own sends once the socket buffers fill:
what a slow or hostile client costs the server is bounded.

| limit (`net::Limits`, HTTP's setting) | when it is reached |
|---|---|
| `max_conns`, and `pool_conns` over every worker on the listener (HTTP: both 65536) | a new connection gets `replies.busy` (HTTP: a 503) and is closed |
| `in_size` (8 KB head + 8 KB body) | input not consumed fills it: closed (HTTP refuses earlier with 431 or 413) |
| `out_queue` (1 MB + 64 KB) | a connection's unsent output past it: closed; HTTP answers a body past 1 MB with 500 (stream it instead) |
| `out_total` (16 MB per worker, in 16 KB chunks) | a producer waits, out of the poll set, until another connection frees some; HTTP answers a body larger than what is left with 503 |
| `idle_ms` (5 s) | closed |
| `read_ms` (5 s from a request's first byte) | `replies.timeout` (HTTP: a 408), closed |
| `write_ms` (5 s with nothing taken) | closed |
| `grace_ms` (10 s after a stop) | still busy: closed |

A stream holds at most one of `P`'s pieces beyond what the socket's
buffers do (HTTP's pieces are 64 KB). Output memory comes from a chunk
slab and input from a slot slab, both grow-only with free lists, and both
stay the worker's until `serve` returns: bounded by `out_total` and by the
most connections that had partial input at once times `in_size`. The
poller's own tables are C heap, doubling as connections are added and
freed when the poller closes. `serve` returns `Stats`: accepted and closed
(equal when it returns), refusals and timeouts by kind, overflows,
contract breaks by `D` or `P`, OS errors, bytes each way, and the most
output memory held at once.

**The 16 KB limit is gone.** The first loop kept at most 16 KB of a
response unsent per connection and closed the connection, counted as a
server error, when more was left; no route then produced more. Now
`/v1/items?repeat=1000` answers about 630 KB built whole, and
`/v1/items/export` streams up to 1,000,000 copies of the catalog, about
64 MB per 100,000, in chunks (to an HTTP/1.0 client, until the connection
closes). `test_api.py` checks a 630 KB and a 12.8 MB response each
followed by a request on the same connection, a slow reader of a stream
while other clients are served (slowest of 20 in 1 ms), a reset 300 KB
into a 64 MB stream, fragmented and split pipelined requests, and a stream
under way at shutdown that is finished before its connection closes.
`net_serve.goose` checks an 8 MB response through a 64 KB queue in order
with the line after it, a 2 MB slow reader, input past `in_size`,
`max_conns`, the idle timeout, 200 connections of churn, a stop with
connections idle and mid-response, and a worker whose output memory is
all held by one unread connection, where the other connection's
producer waits parked and both get every byte.

**Blocking handlers.** In the pool a handler that blocks stalls every
connection of its worker: there is no preemption and no scheduler moving
connections between workers, unlike Go's goroutines, and an abort ends
all of a worker's connections, not one request. The thread-per-connection
mode is the tested alternative: the acceptor detaches each connection, a
contained thread adopts it and runs the same loop over it alone, so its
handler may block and an abort ends only that connection, at about
130 KB resident and two threads per connection (section 2).

### Readiness backends: epoll, kqueue and AFD

`poll` and `WSAPoll` scan every socket of a poller on each wait, so a
worker's wait costs what is open, not what is ready. A poller now waits
with the platform's own readiness interface, behind the same `wait` and
`want`, level-triggered as the loop expects (it reads once per report and
asks again):

- **Linux: epoll**, one instance per poller (`epoll_create1`), a socket
  added, changed or removed (`EPOLL_CTL_*`) when what the loop waits for
  on it changes.
- **macOS and the BSDs: kqueue**, a read and a write filter per socket,
  their reports for one socket merged into one event.
- **Windows: the AFD poll** that Windows' own sockets are built on
  (`IOCTL_AFD_POLL` on `\Device\Afd`, reached through `NtCreateFile` and
  `NtDeviceIoControlFile` in ntdll, on the socket's base handle from
  `SIO_BASE_HANDLE`), each socket's poll completing on one I/O completion
  port per poller: the technique of wepoll and of Rust's mio, which tokio
  is built on. A completed poll is resubmitted at the next wait, and a
  change of what is waited for cancels the one outstanding, so the reports
  are level-triggered as `WSAPoll`'s are. Closing a poller cancels its
  polls and waits up to a second for their completions before it frees
  their state, which the kernel writes into until then.

The poller's array stays the registry either way (an entry per socket with
what it waits for), mirrored in the kernel; a kernel report carries the
connection's index, or a watched listener's handle with a flag bit, which
the wait maps back to a checked handle, so a report for a connection closed
earlier in the same batch reaches nothing. A `want` for what the socket
already waits for makes no system call, which the loop does after most
requests. A wait looks for closed listeners it watches only when one has
closed since it last looked (a process-wide counter), not on every wait.
`GUST_NET_POLL=1` keeps `poll` and `WSAPoll` for a process, which is how
the portable path is still tested (`net_serve` passes either way).

Two findings on the way, both about workers sharing a listener:

- **Accepts were not shared.** Every worker is woken for a waiting
  connection and the first takes it; a busy worker reaches the listener
  less often, so at 50,000 connections, with the example's limit split
  evenly (65,536 / 6 workers), two workers filled up and turned away 27,364
  connections while others held a few hundred (per-worker counts from a
  debug build: one worker took 1,061, the rest 7,800 to 10,922). `serve`
  now takes at most 32 connections a turn, and `Limits.pool_conns` is a
  limit over every worker on the listener, counted by the runtime: each
  listener keeps an atomic tally of its connections still open, wherever
  they are (`net::accepted_open`), which `stdlib/http.goose` uses in
  place of the split. The 50,000-connection runs below turned none away.
- **The tally must count only connections.** A first version added to
  the tally before each `accept` and took it back when there was none, so
  a worker's accept that found nothing made another worker's count look
  one higher for a moment, and `net_serve`'s pool check failed: now only an
  accepted connection is counted, while the listener is held.

IOCP proper (completion of each read and write, not readiness) is still
not used: the OS owns a buffer until its operation completes, which would
move `serve`'s slots and chunks into the runtime, lent per operation.
Readiness through AFD gets what IOCP was wanted for here, a wait whose
cost is what is ready.

### At scale: idle connections

`examples/api/scale.py` (Windows) and an equivalent driver on Linux run the
pool under the mixed load on 64 keep-alive connections beside N idle
keep-alive connections, each sending a request every 2 s
(`loadgen -idle N`), with the platform's backend and with `poll`/`WSAPoll`
(`GUST_NET_POLL=1`). Three reps of 10 s per cell, medians; the range is the
lowest and highest rep; latency is the 64 measured connections'.

**Linux** (WSL2, kernel 6.6, on the Windows machine of section 5; 6
workers on CPUs 0–5, the load generator on CPUs 6–11; the server built
from gust's C with gcc 15.2 at `-O2`; 26 October 2026,
`examples/api/results/2026-10-26-scale-linux.json`):

| idle connections | backend | req/s (range) | p50 µs | p99 µs | p99.9 µs | server CPU µs/request | peak RSS MB |
|---:|---|---:|---:|---:|---:|---:|---:|
| 0 | epoll | 440,983 (420,954–450,847) | 118 | 507 | 1,250 | 9.2 | 3.4 |
| 0 | poll | 465,849 (456,835–474,746) | 111 | 497 | 1,343 | 9.7 | 3.4 |
| 1,000 | epoll | 447,761 (444,196–465,870) | 118 | 488 | 1,127 | 9.2 | 3.6 |
| 1,000 | poll | 389,119 (377,513–405,442) | 133 | 546 | 1,152 | 14.4 | 3.6 |
| 10,000 | epoll | 420,719 (412,921–447,834) | 122 | 543 | 1,345 | 10.0 | 5.7 |
| 10,000 | poll | 50,590 (49,035–56,563) | 1,241 | 2,292 | 3,649 | 121.3 | 5.6 |
| 50,000 | epoll | 381,545 (364,459–388,938) | 140 | 574 | 1,119 | 12.4 | 13.5 |
| 50,000 | poll | 13,288 (12,551–13,469) | 4,691 | 8,606 | 15,425 | 1,309.4 | 14.8 |

**Windows** (11 Pro, the machine of section 5; the server on CPUs 0–7 with
its default workers, the load generator on 8–15; `gust build --cc clang`,
clang 22.1.8; the same day,
`examples/api/results/2026-10-26-scale-windows.json`). 12,000 is the top:
loopback clients there share Windows' 16,384 ephemeral ports, and no
system setting was changed to raise that.

| idle connections | backend | req/s (range) | p50 µs | p99 µs | p99.9 µs | server CPU µs/request | peak RSS MB |
|---:|---|---:|---:|---:|---:|---:|---:|
| 0 | AFD | 160,194 (143,333–172,939) | 315 | 2,352 | 5,307 | 36.1 | 6.0 |
| 0 | WSAPoll | 160,920 (159,589–179,706) | 288 | 2,449 | 5,319 | 36.1 | 6.1 |
| 1,000 | AFD | 156,687 (140,801–169,318) | 316 | 2,406 | 4,957 | 37.3 | 6.8 |
| 1,000 | WSAPoll | 144,173 (125,033–150,908) | 346 | 2,161 | 4,496 | 44.7 | 7.9 |
| 5,000 | AFD | 157,021 (153,450–159,663) | 315 | 2,345 | 4,821 | 38.5 | 10.4 |
| 5,000 | WSAPoll | 50,334 (45,314–54,708) | 1,111 | 3,616 | 6,028 | 157.6 | 13.5 |
| 10,000 | AFD | 150,405 (146,821–166,980) | 328 | 2,344 | 4,700 | 40.3 | 13.8 |
| 10,000 | WSAPoll | 19,295 (17,744–20,979) | 2,986 | 7,748 | 13,052 | 512.1 | 18.7 |
| 12,000 | AFD | 134,054 (132,307–150,182) | 325 | 2,846 | 5,768 | 41.3 | 14.9 |
| 12,000 | WSAPoll | 13,602 (12,764–14,007) | 4,190 | 10,949 | 24,841 | 885.3 | 20.7 |

Other programs used 6–50% of the machine's CPU in the second before a run
(nothing was closed for these runs), which is most of the spread of the
ranges.

What the runs show:

- **The wait no longer costs what is open.** With epoll, 50,000 idle
  connections cost the measured load 13% of its throughput (441,000 to
  382,000 requests a second) and p99 stays under 0.6 ms; with `poll` the
  same load falls to 13,300 a second, 29 times less, every wait scanning
  about 8,300 sockets per worker, and the server spends 1.3 ms of CPU per
  request. At 10,000 the difference is 8.3 times.
  On Windows, AFD keeps 94% of the throughput at 10,000 idle connections (160,000 to 150,000 a second, p99 unchanged at 2.3 ms) where WSAPoll keeps 12%, 7.8 times less; at 12,000, 134,000 against 13,600.
- **Memory per idle connection** in the server is about 0.2 KB on Linux
  (13.5 MB peak at 50,000 against 3.4 MB at none): the loop's record and
  the poller's entries; on Windows about 0.85 KB of private memory, the
  AFD poll's state per socket included, against 1.4 KB with WSAPoll. The kernel's socket buffers are not in that and
  are most of what an idle connection costs.
- **With no idle connections `poll` is ahead on Linux**, by 5% (466,000
  against 441,000; the ranges do not overlap): over a dozen sockets per
  worker its scan is cheap, and epoll's bookkeeping is not. On Windows the
  two are level there (160,194 against 160,920, ranges overlapping). A hybrid that
  waits with `poll` below some count was not built; nothing here measures
  where the two cross but that it is below 1,000.
- **Nothing refused, nothing failed**: no request errors and no idle
  connection's ping failed in any run; every connection was accepted.
- Criterion (b) of section 7 asked for 1,000 active connections; these
  runs have 64 active, the load of section 5, so that the idle ones are
  the only change. The share of the server's CPU in the wait was not
  profiled; CPU per request, the same work otherwise, says it instead.

### Before and after

`python examples/api/bench.py --reps 5 --seconds 10 --kinds gust --before
old_api.exe`, on 26 October 2026: the pool server built from the tree
before the module (commit `6008946c`, the example's own `net.c` and
`serve_loop`) and after it, both by the same compiler with clang at `-O2`,
`--workers 8`, on the same CPUs and load as section 5, alternating which
runs first. Raw runs are in
`examples/api/results/2026-10-26-net-before-after.json`; medians, with the
lowest and highest of the five reps:

| scenario | server | req/s (range) | p50 µs | p99 µs (range) | p99.9 µs (range) | peak RSS MB | other CPU before |
|---|---|---:|---:|---:|---:|---:|---:|
| mixed, 16 connections | before | 122,376 (109,028–137,820) | 104 | 535 (386–695) | 1,278 (806–1,657) | 5.8 | 41% |
| mixed, 16 connections | after | 127,221 (98,860–137,760) | 102 | 454 (395–914) | 1,061 (830–2,551) | 5.9 | 21% |
| mixed, 64 connections | before | 145,268 (140,695–155,082) | 305 | 2,791 (2,479–3,560) | 6,916 (5,198–10,540) | 5.9 | 32% |
| mixed, 64 connections | after | 155,647 (149,187–162,156) | 309 | 2,430 (2,144–2,903) | 6,390 (5,456–9,882) | 6.0 | 31% |
| mixed, 256 connections | before | 165,271 (130,331–179,084) | 1,271 | 5,556 (4,454–11,047) | 11,251 (8,543–39,762) | 6.0 | 15% |
| mixed, 256 connections | after | 160,096 (155,482–170,654) | 1,266 | 5,684 (5,009–6,349) | 12,856 (10,449–14,799) | 6.2 | 26% |
| health, 64 connections | before | 161,934 (146,583–169,498) | 283 | 2,440 (2,229–2,898) | 7,844 (4,948–9,843) | 5.9 | 17% |
| health, 64 connections | after | 156,213 (143,199–161,164) | 290 | 2,517 (2,429–3,539) | 6,006 (5,478–8,740) | 5.9 | 38% |
| order, 64 connections | before | 160,913 (146,519–169,142) | 304 | 2,336 (1,998–3,112) | 8,190 (6,366–8,639) | 5.9 | 31% |
| order, 64 connections | after | 151,993 (143,309–164,540) | 307 | 2,896 (2,233–3,141) | 8,436 (6,551–8,559) | 6.0 | 26% |
| mixed, 64 connections, 20k req/s fixed rate | before | 20,000 | 85 | 198 (175–376) | 494 (271–3,519) | 5.9 | 18% |
| mixed, 64 connections, 20k req/s fixed rate | after | 20,000 | 86 | 209 (181–419) | 598 (335–1,832) | 5.9 | 23% |
| mixed, new connection per request, 16 clients | before | 3,977 (1,251–7,446) | 1,679 | 9,473 (2,977–34,790) | 20,819 (13,083–61,029) | 5.9 | 71% |
| mixed, new connection per request, 16 clients | after | 6,903 (1,167–7,647) | 1,451 | 5,485 (2,967–39,504) | 22,942 (15,046–71,460) | 6.0 | 29% |
| mixed, 64 connections, 32 slow readers | before | 99,958 (77,250–117,641) | 507 | 1,865 (1,197–6,090) | 3,489 (1,914–20,378) | 7.1 | 43% |
| mixed, 64 connections, 32 slow readers | after | 111,426 (67,572–128,064) | 470 | 3,190 (1,350–10,196) | 10,717 (2,421–21,645) | 7.3 | 75% |
| large (63 KB built whole), 64 connections | after | 19,087 (18,245–20,955) | 3,084 | 8,628 (7,467–10,734) | 14,473 (10,353–21,144) | 6.8 | 19% |
| stream (640 KB chunked), 16 connections | after | 2,081 (1,707–2,133) | 6,993 | 24,028 (15,187–38,450) | 35,664 (29,511–49,942) | 7.7 | 26% |

"Other CPU before" is the machine's CPU use in the second before each run,
the median over the reps, from programs this benchmark does not control
(no application was closed for it): it ranged from 11% to 100% in single
runs, against 23.5% measured once before the benchmark started, and the
churn and slow-reader runs saw the most. What the runs show:

- **No difference beyond the noise.** In every scenario both servers ran,
  the after server's range overlaps the before server's, in throughput
  and in the tails. Its medians are higher in four (mixed at 16 and 64
  connections, churn, slow readers) and lower in four (mixed at 256,
  health, order, the fixed rate's p99.9); none of these is a measured
  change, and a regression smaller than about 10% would not show at this
  level of background load.
- **Memory is the same where the work is the same**: 5.8–6.2 MB peak in
  the closed-loop scenarios, 2 KB resident per idle connection (500 idle
  connections), and page faults per thousand requests at or below the
  before server's.
- **What only the after server does stays bounded.** 63 KB responses built
  whole, 19,000 a second (about 1.2 GB/s), peaked at 6.8 MB; 640 KB
  streamed responses, 2,100 a second (about 1.3 GB/s), at 7.7 MB, with 16
  of them in flight. Both are limited by the server's CPU (85%), which
  spends it encoding the JSON, not by the loop. The before server could
  not send either: no route produced more than 16 KB, which it would have
  closed the connection on.
- **Slow readers.** With 32 clients pipelining requests and reading 4 KB
  every 50 ms beside 64 normal connections, both servers held the slow
  ones' output back without failing anyone (no errors, the slow readers
  receiving the same amount); the after server's p99.9 median is higher
  (10.7 ms against 3.5 ms), with ranges that overlap (2.4–21.6 against
  1.9–20.4 ms) in the runs where other programs used the most CPU (75%
  median), so this one is the first to measure again on a quieter machine.
- **Errors**: none for the after server; one for the before server in one
  churn rep, a load-generator dial failure, as in section 5.

## 7. Roadmap for backends, in order

Ranked by measured impact against effort; each names its evidence. The
`net` module, first here before, is done (section 6).

1. **Next milestone: the network in hosted CI, and measured at scale.**
   Exit criteria: (a) the hosted CI run on Windows, Linux and macOS is
   green with `stdlib_net`, `net_serve`, the API and echo examples, and
   the sanitize profile runs both examples under ASan and UBSan **(met:
   run 40341927278 on commit `420714c5`, section 8)**; (b) a
   scaling run of the pool at 1,000, 10,000 and 50,000 mostly idle
   connections with 1,000 active, reporting throughput, p99 and p99.9,
   memory per connection and the share of server CPU in `poll`/`WSAPoll`
   from a profiler **(met with two differences, section 6: 64 active
   connections, the load of section 5, so that the idle ones are the only
   change; and the wait's cost read from the server's CPU per request, 13
   times higher with `poll` at 10,000 on Linux, not from a profiler; on
   Windows to 12,000, loopback's port limit there)**; (c) epoll, kqueue or
   Windows' AFD poll added behind `wait` and `want` only where (b) shows
   the wait taking more than a fifth of the server's CPU at 10,000
   connections, the same tests passing with it, and section 6's scenarios
   no slower beyond their run-to-run range **(met: all three added, the
   `net` fixtures passing with them and with `GUST_NET_POLL=1`; at no idle
   connections AFD is level with WSAPoll, and epoll 5% behind `poll` on
   Linux, outside its range; kqueue is built and tested only by CI's
   macOS job, which has not run it yet)**.
2. **Cancellation the runtime owns**: a stop token that `qget`,
   `thread_wait` and the `net` waits return on, and a deadline per request
   a handler can see (Go's `context`, Zig 0.16's `cancel`). Today a stop
   reaches a loop within 50 ms but cannot interrupt a handler. Effort:
   runtime and compiler (new builtins). **(In part: a stop now wakes every
   epoll, kqueue and AFD poller's wait at once; `net::Ctl` carries
   `stopping` and a per-request `deadline` from `Limits.request_ms`, which
   HTTP hands its handlers as `Request.deadline` and `Request.stopping`, for
   them to keep to; nothing interrupts a handler that does not look, and
   `qget` and `thread_wait` still return at their timeouts.)**
3. **M:N workers on the data stacks**, so handlers may block at event-loop
   cost. A worker's data lives on its data stacks, so switching workers in
   user space moves no data. This is what would retire the thread-per-
   connection mode. Effort: large (runtime research); a milestone of its
   own, as is work stealing between workers.
4. **Freeze-then-share** (ROADMAP Phase 4): data built at startup and read
   by every worker in place. Today it must be a compile-time `const` or be
   copied into each thread. A milestone of its own.
5. **Addressed mailboxes**, so a worker can ask another and get the answer
   itself; without them mutable shared state (a store, a cache, a rate
   limiter) has no place in a share-nothing server.
6. **Containable out-of-memory**: a data stack the system will not commit,
   in a contained worker, could end that worker rather than the process.
7. **`const if` and field attributes**, so JSON needs no opt-in line and
   validation can be declared on fields, as FastAPI does with annotations;
   **a router** from an enum of routes expanded with `const for`.
8. **The rest of HTTP and networking**: chunked request bodies, a client,
   IPv6 and DNS, TLS in a package that wraps a C library, HTTP/2 later;
   logging, metrics and dates in the standard library.
9. **A non-blocking `thread_wait`.** It would halve the thread-per-
   connection mode's memory (measured, section 2); nothing else here needs
   it, and the runtime now closes an ended thread's sockets itself.

## 8. What is not settled

- **Where it has run.** Hosted CI, run 40341927278 on commit `420714c5`,
  green in every job: windows-latest (MSVC 19.51), macos-latest (clang
  21.0) and ubuntu-latest (gcc 13.3), each running the whole suite, with
  both `net` fixtures in generated C at `-O0` and `-O2` (and
  `stdlib_net` in the TinyCC JIT) and the API and echo examples, normal
  and `--checked`, the API in both ways to serve; and the Linux sanitize
  profile (clang 18.1.3, ASan and UBSan, no JIT), which runs the fixtures
  and builds and drives both examples with its sanitizer flags. On macOS
  that includes the API's resets mid-response and mid-stream, which a
  `SIGPIPE` would have ended the server on, so the `SO_NOSIGPIPE` path
  holds there. The handle-count check of `test_api.py` needs `psutil`,
  which CI's Python does not have: it ran on Windows here only (64
  handles before and after). The run before it, 40339108305 on
  `671d700e`, failed on two faults these tests found, both fixed: on
  macOS every TinyCC JIT program failed to compile, the stop flag using
  atomics TinyCC does not have on POSIX (`4b60ae52`; Linux has the same
  backend and would have failed alike); and Linux stopped before its tests
  at a format check, merged from Goose, that called `build/goose`
  (`420714c5`). Locally as well: Windows 11 with clang, MSVC and the JIT,
  and Linux in WSL2 (kernel 6.6, gcc 15.2, not native Linux), where the
  C gust wrote for the fixtures and both examples was built with the
  sanitizers and run, and the vendored TinyCC was built to reproduce and
  check the JIT fault. The scale runs of section 6 are the only Linux
  benchmark, in WSL2, not native Linux; nothing was benchmarked on macOS.
- **One machine, loopback.** A network between client and server adds
  latency that hides some of these differences.
- **Gust's allocation count is established from the generated C, not
  counted.** The request path (`stdlib/http.goose`'s protocol and handlers, and
  `net::serve` around them) contains no call to an allocator: its slot and
  chunk slabs grow on a data stack only when more connections than before
  have input or output pending at once, and the poller's C tables grow by
  doubling when more connections are open at once than before. Page
  faults per request are the runtime-side check. Go's numbers are its
  runtime's counters.
- **The Gust server is a subset of `net/http`**: no chunked request
  bodies, no `Date` header, no `context` per request, no HTTP/2. The
  minimal-HTTP Go server is the like-for-like comparison.
- **Untested paths.** The HTTP 503s for `max_conns` and for a worker out
  of output memory are not reached by `test_api.py` (the loop's busy reply
  and its parking are, in `net_serve.goose`); nothing exercises IPv6
  (unsupported); the kqueue backend (macOS) has not run at all until CI's
  macOS job runs it, and has never been under load.
- **Out of file descriptors.** On Linux and macOS the runtime holds one
  descriptor in reserve: an accept refused for want of descriptors spends
  it to take the waiting connection off the queue and drop it, so its
  client hears at once, and `serve` drains up to 64 such before resting the
  listener 100 ms rather than spin (`Stats.accept_failures`).
  `test/net_fd_limit.py` runs the echo example with a 40-descriptor limit
  and 120 clients: some are served, the rest dropped, the worker uses
  little CPU while out of descriptors, and it serves again once they go;
  against the runtime before this change, 84 of the 120 were left hanging
  in the queue and none dropped. On Windows, where nothing is dropped, the
  listener rests the same way; that path is not tested.
- **Not production-ready.** The module is new: its tests pass on three
  platforms in CI and under the sanitizers, but it has no TLS, no IPv6,
  has been run at scale only on loopback (50,000 idle connections in WSL2,
  12,000 on Windows), has served no traffic but these tests' and
  benchmarks', and was measured on one machine only; the comparisons with Go and Rust say how these programs
  did on these workloads, not that Gust is faster or as safe in general.
