# Next performance designs

Designs that the comparison against Rust, Go and Goose points to, with
the measurements behind each, and the tools the measurements came from.
All of them are built: the dictionary in groups, the keyed hash, the queue with no
lock between its ends and MSVC's AVX versions.

## A dictionary in groups (Swiss table)

**Where it falls short.** `stdlib/dictionary.goose` is open addressing with
linear probing over 17-byte slots (key, value, one tag byte) at most 2/3
full. Rust's hashbrown and Go's maps (`internal/runtime/maps` in Go 1.26's
source, after Abseil's Swiss tables) keep a control byte per slot in groups
of 8 or 16, compare all of a group's tags with the key's in a few
instructions, and fill to 7/8. Two results follow from the difference:

* A table that grows from empty doubles at 2/3 where Rust's does at 7/8: 885k
  integer entries took 41 MB in Gust and 33 MB in Rust (the growth test
  beside `compare/`), the doubling having come sooner.
* On the faster of the hosted runner types, `compare/`'s hashmap ran at
  1.11x Rust's time (Bench run 41257002851) though its hash is cheaper
  than Rust's SipHash: cachegrind counted 0.46x Rust's instructions and
  3.3x its mispredicted branches (8.95 million against 2.73). Half the
  lookups miss, and each probe's walk ends where an unused slot happens to
  be, a branch the predictor cannot learn; a group's test settles eight
  slots with one.

**The design.** The slot array becomes an array of groups, Go's layout:

```goose
struct dictionary_group<K, V> { ctrl: u64, slots: dictionary_slot<K, V>[8] }
struct dictionary<K, V> { count: i64 = 0, groups: dictionary_group<K, V>[>..<] = [] }
```

One resizable array, so the dictionary stays an ordinary struct with a
resizable tail (§3.4) and references into it keep their rules (§5.2). A
control byte is 0x80 empty, 0xFE deleted, or the hash's top seven bits.
A lookup takes the key's home group from the low bits, matches the tag
against the control word with the SWAR test (`(w ^ tag * 0x0101..01) - 0x0101..01
& ~w & 0x8080..80`, false positives settled by the key comparison) and stops
at a group with an empty byte. Growth at 7/8 doubles the array in place,
as today: each entry's new group is its old one or that plus the old
count, by one more bit of hash. Removal marks a slot empty where its group
has an empty byte already, a tombstone otherwise, and growth or a rehash
in place clears the tombstones.

**What to measure.** `compare/`'s hashmap and strings (time, peak memory,
instructions under cachegrind), and the growth and removal test with
integer and string keys, against today's dictionary and Rust's.

**Built** (MODIFICATIONS.md has the measurements). Two departures: a
growth rehashes every entry by hashbrown's in-place walk rather than
splitting each group in two, since an entry may sit past its home group;
and a match's slot comes from a multiply, about as fast as `ctz` under the
C compilers and several times faster under TinyCC and MIR. Lookups in a
large table cost more than before where the old table had just doubled:
two cache lines (control word, then slot) at up to 7/8 load against one at
as little as 1/3.

## A hash that is keyed only when it has to be

**Where it falls short.** A hash that is not keyed can be attacked: for 24-byte
keys, the second word can undo what the first did to the state, so any number
of keys hash alike, and each insert then walks past all the keys before it.
16,000 such keys took a second, and 64,000 twenty times that.

**What others do.** Rust's `HashMap` hashes every key with SipHash-1-3 under a
random key, and Go's maps use a random seed with AES or wyhash. Both pay for it
on every operation, in programs nobody is attacking, and their order changes
between runs.

**The design.** Keep the fast hash and watch for the attack. The probe already
knows what it has seen: how many full groups it passed, and how many keys it
compared in vain, which for ordinary keys is one tag in 128. An insert that has
seen too much gives the table a seed, nonzero from then on, and rehashes it. A
table with a seed hashes keys under it: SipHash of the bytes, and of the
combination in `hash_combine`, through a global that says which seed applies,
and SipHash again over the result, which is all an integer needs, its plain
hash being a bijection.

**What it costs.** One load and one test per operation, and that only because
`cold fn` keeps the branch to the slow path from costing registers on the way
past it. The limit has to sit above what ordinary keys reach: in tables at
their fullest, 60% of probes pass a group and the longest passed 36 in 40
million, so a limit of 32 groups would have gone off on its own. So a group
counts 1 and a failed compare 4, and the limit is 64: 16 keys that collide
outright reach it, and so do 64 groups of keys that only agree in their low bits.

**What it leaves.** Keys built to stay just under the limit, in sets of fifteen
that collide, cost at most that many compares per operation; there is nothing
quadratic left to reach for. A key type whose `hash` does not use `hash_combine`
can still be made to collide, since nothing gives it the key.

## A queue with no lock between its sender and its receivers

**Where it falls short.** A typed queue has one lock, which the sender and the
receivers all take for every message, so its cache line goes from core to core and
back for each. With a sender and three receivers on a 4-CPU Linux runner a million
messages take 0.16 s, against 0.05 s for Go and Rust. Pinned to the two hyperthreads
of one core the same run takes a quarter of what two cores do on Intel, which is what
shows that this and nothing in the kernel is the cost: the run spends no time in the
system. Real jobs take long enough that this rarely shows.

**What others do.** Rust's channel is a linked list of blocks with its ends apart, and
Go's sender fills a buffer while the receivers are parked. Neither moves a line per
message.

**The design.** Two locks, as in Michael and Scott's two-lock queue: one for the
sender's end and one for the receivers', on lines of their own, each end keeping its
last sight of the other's index and looking again only when that runs out. With 8-byte
messages one slot line moves per eight messages. Growth, the change to nodes and a
limit's change take both, receivers' first. A sleeping receiver needs a store and a fence
on each side, so that a send can't miss it, which is the part to get right: every
interleaving of it was explored for up to three threads on each end, about 608,000 states,
along with three broken forms of it, each of which the check caught. Under
ThreadSanitizer the stress test `test/threads/queue_stress.goose` reports nothing.

**What it gave.** One lock against two on the same 4-CPU runner, three runs on three
kinds of CPU: a sender and three receivers takes 0.73x to 0.83x of the time, and messages
back and forth 0.64x to 0.75x, which is level with Go on some CPUs (1.04x) and 2x on others.
That is less than the design hoped
for. The sender and three receivers are still 1.2x to 1.8x Rust and 1.7x to 2.1x Go, since
the receivers pass the one lock on their end among them. So this closes part of the gap and
not all of it. A thread also remembers the mailboxes it has asked for, which never
move, rather than taking a lock every thread of that type asks for each message.

Those runs were all on Linux. On Windows, on one machine in 25 rounds (probe run 41531683290),
clang's build of a sender and three receivers is level at its fastest (1.01x) and 1.13x at its
median, MSVC's 0.95x and 1.20x: the two-lock queue's times spread more there, 0.056 s to 0.094 s
against 0.055 s to 0.061 s, and even its slowest is ahead of Rust's fastest, 0.112 s, and Go's,
0.115 s. Messages back and forth take 0.49x with clang and 0.94x with MSVC (medians 0.56x and
0.77x). A full Bench run reads differently, 1.28x Rust and 0.82x Go, on a runner where Rust's own
time was 0.070 s, which is why these comparisons are made on one machine.

**What that spread was: receivers looking at the sender's line.** A receiver waiting for a message
read `tail` on every look, though it had often already seen that more were there, and each read
takes that line from the sender. So a receiver now looks first at what the last take saw of `tail`,
which lives on its own end's line, and goes to `tail` only once that has run out. It changes only a
hint that the lock then decides, so the sleeping protocol is as it was. On one machine in 25 rounds
(probe run 41539412137), a sender and three receivers on Windows went from a median of 0.071 s, with
a slowest of 0.097 s, to 0.027 s, with a slowest of 0.028 s, against the one-lock queue's 0.058 s,
Rust's 0.115 s and Go's 0.118 s. On Linux, where the receivers' lock is what limits it, 0.121 s went
to 0.112 s, against Rust's 0.063 s and Go's 0.054 s. Messages back and forth moved 1.02x on Linux and
0.95x on a Windows PC. On that runner they read 1.18x, but so did variants that do not touch them
(1.14x to 1.19x), so that row was its reference's luck. A full Bench run (41543135535) agrees: on
Windows 0.26x Rust and 0.25x Go, and on Linux 1.31x and 1.70x, on another kind of CPU.

Tried on the way, and dropped:

- Not signalling a receiver already signalled and not yet back, which the first counters here
  pointed at, on a PC where a slow run had thousands of such signals. It passed a check of every
  interleaving for up to three threads on each end, 1.2 million states, but on the runners it gave
  under 2%: one run with 172,852 such signals was not slow, and the slow ones were those with
  receivers really asleep some 3,000 times, which the look at `tail` now avoids.
- Polling less often the longer a receiver has waited: 0.86x on Windows' spread, but messages back
  and forth 1.16x to 1.53x on Windows and 1.35x on Linux.
- Spinning four times as long before sleeping (0.95x) and backing off an eighth as long after a lost
  race (0.85x on Windows, 1.00x on Linux): neither near what the look gave.
- Trying Windows's lock before waiting on it, as the POSIX path does: 1.26x, worse.

## What Zig and Nim had to offer

Read in November 2026 from their sources, for Gust's remaining gaps, and measured where a
number could decide it: patches to the runtime's C on one 4-CPU Linux runner.

**Taken: Zig's C compiler.** `gust build --target` (MODIFICATIONS.md). Zig carries a C library
for every target, and Gust compiles to C, so it gets static executables and cross-compilation
without a backend of its own. 88 test programs pass on each of Linux x86-64 and arm64 with
musl, Windows and macOS, on machines of that kind.

**Measured, and not taken.**

- Reading a lock before trying it, as Zig's mutex notes that a failed try still takes the line
  (glibc's lock word read directly): a sender and three receivers 0.94x, within the noise of
  these runs, for a dependence on glibc's internals.
- Waking one sleeping thread and leaving it to wake the next, as Go's scheduler does:
  a hundred workers at once 1.25x, worse.

**Built: buffering stdout.** C, Nim and Python buffer it when it is not a terminal. 300,000 prints
to a file went from 0.211 s to 0.009 s, and to a pipe from 0.131 s to 0.007 s, against Rust's 0.252
and 0.171 and Go's 0.312 and 0.205. The runtime used to write each print on purpose, so that output
was not lost to an abort or a killed run and stayed in order with stderr. So the buffer goes out at
an abort, a fault, SIGINT, SIGTERM and SIGHUP, before stderr and before a thread blocks, and a print
more than 50 ms after the last write takes it along. That leaves one loss: a process killed outright
while printing loses what is in the buffer. A terminal, `GUST_UNBUFFERED` and foreign C in the
program keep a write per print.

Signals were the hard part, and `scripts/stdout_tests.py` found three mistakes in the first versions,
none of them seen in ordinary use: a signal during a write printed the buffer twice, a fault on
another thread cut a write off in a line, and a program stuck writing to a reader that had stopped
no longer ended on a signal (`prog | less`, then Ctrl-C). A write in progress now finishes first, and
a signal allows a stalled reader 2 seconds. The signals' tests repeat at varied moments, since the
first of these appeared on one machine in three, by chance of timing.

**Read, and not applicable, for these reasons.** None of these was measured.

- Nim's ARC and ORC: reference counts, and a cycle collector, for what Gust's regions and data
  stacks do without either.
- Zig's SmpAllocator, Nim's TLSF allocator and Weave's memory pools: Gust's hot paths do not call
  an allocator.
- Malebolgia's running a task inline when the pool is busy: a worker may block on what another
  has yet to do, and `parallel for` has its caller take chunks already.
- Weave's batches of messages and work stealing: a receiver holding messages it has not asked for
  can deadlock when it then waits for something another thread can only do with one of them.
- Dragonbox and Schubfach, in Nim's float text: no slow path, for a table of about 10 KB, where
  Gust's already beats Rust and Go by 12 to 17% and falls back on under 1% of values.

**Read, and left open.** Zig's MultiArrayList, a struct array stored field by field, would suit
what lanes do, as a library type. Its ArrayHashMap, a dictionary in insertion order, would suit
JSON objects, and costs memory rather than saving it. Its pdqsort's block partition was not
measured against Gust's branchless one, which is what integer sorting would try next.

## MSVC's AVX versions, in a translation unit of their own

**Where it falls short.** MSVC builds no simd versions: it has no
per-function target, so the lanes run as pairs of SSE2 registers, and
`compare/`'s raytrace stays at 1.2x to 1.4x Rust's time (clang 0.81x, gcc
0.83x with its AVX versions). `compare/sample_profile.py` shows the time
90% in the two sphere loops (`nearest`, `any_hit`).

**What did not work, and why.** AVX intrinsics in a translation unit
compiled without `/arch:AVX`: built that way, `test/codegen/lanes.goose`
printed a wrong sum of the y and z components (the x components, and the
body left to the scalar loop, right). The same wrong sum came back in the
unit built with `/arch:AVX2`, at `/O2` and `/O1` but not at `/Od`, `/Og` or
`/Ob0`: MSVC 19.50, inlining `gs_l4x_get_avx` (a lane out of a vector) as
a `__forceinline` function, read the lanes of a lane struct's second and
third members wrongly. Under MSVC the AVX lane operations are therefore
macros (`src/runtime/runtime_ext.h`), the extraction AVX2's `vpermpd` with
the lane as its immediate, and the sum is right.

**The design, as built.** The program's C compiled twice: once as now
(`/DGS_TU_BASE`), and once with `/arch:AVX2 /DGS_TU_AVX2`, keeping only
the AVX2 versions, with external linkage, which the first one's dispatch
calls (`__cpuidex`, `_xgetbv`). Every mutable datum the generated C defines
(the globals `gs_globals_main`, the data stacks' thread-locals, the
long-distance return's channels, the queues, the dynamic externs'
pointers, the init flag) is declared `GS_DATA`, which defines it in the
first unit and declares it `extern` in the second, so both units share one
of each; code and constant data are each unit's own copy. The build
(`src/build.h`) compiles the second unit only for MSVC on x86-64, for a
program with simd versions and no C header (whose static data the second
unit would copy), and not `--standalone`.

**What that cost, and the fix.** An external `gs_stks` is one MSVC reloads
after every store that might have changed it: `compare/`'s A* search
(`solve`), whose code is in the first unit and unchanged, took 25 more
instructions and ran 6% slower. A thread's block of stacks does not move
while Goose code runs on it, so in the two units a function that indexes
the block takes it into a local first (`GS_STKS_LOCAL`, `runtime.h`), and
`solve` is back to its old code (1.02x, within the spread).

**Measured** (`compare/quiet_bench.py`, 15 rounds, CPU cycles against
Rust's `compare/build/rust-o3-lto.exe`, MSVC 19.50, the same checksums for
all eight benchmarks): raytrace from 1.20x to 1.00x, n-body from 0.67x to
0.49x; the other six within their spread. "Before" is the same C compiled
as one unit, as `gust build` did. `compare/sample_profile.py` puts
raytrace's time 84% in `nearest_g_simd1` and `any_hit_g_simd1`, and 3.5% in
their dispatch, and n-body's 99.8% in `run_g_5_simd1`.

## Tools

* `compare/quiet_bench.py`: A/B timing on a busy desktop. Each run is pinned
  to the least-used physical core, the builds interleave in a random order
  per round, and the median of each round's paired ratio is reported with
  its spread, in time and in CPU cycles (`QueryProcessCycleTime`).
* `compare/sample_profile.py`: a sampling profiler for Windows that needs no
  install or privilege: the main thread's instruction pointer about once a
  millisecond, mapped to functions through the linker's `/MAP` file.
* `compare/ci_measure.py --instructions`: instructions and mispredicted
  branches under cachegrind on the Linux runner, exact where the times move
  with the runner's load; beside them peak memory, executable sizes, build
  times and a resident-memory breakdown by mapping.
