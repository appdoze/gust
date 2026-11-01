# Next performance designs

Two designs that the comparison against Rust, Go and Goose points to, with
the measurements behind each, and the tools the measurements came from.
Neither is built yet.

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
  1.11x Rust's time (Bench run 41256033182) though its hash is cheaper
  than Rust's SipHash: half its lookups miss, and a miss walks slots until
  an unused one.

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

## MSVC's AVX versions, in a translation unit of their own

**Where it falls short.** MSVC builds no simd versions: it has no
per-function target, so the lanes run as pairs of SSE2 registers, and
`compare/`'s raytrace stays at 1.2x to 1.4x Rust's time (clang 0.81x, gcc
0.83x with its AVX versions). `compare/sample_profile.py` shows the time
90% in the two sphere loops (`nearest`, `any_hit`).

**What does not work.** AVX intrinsics in a translation unit compiled
without `/arch:AVX`: built that way, `test/codegen/lanes.goose` printed a
wrong sum of the y and z components (the x components, and the body left to
the scalar loop, right), the mark of 256-bit registers spilled through
128-bit moves.

**The design.** The program's C compiled twice: once as now, and once
with `/arch:AVX2 /DGS_TU_AVX2`, keeping only the AVX2 versions, with
external linkage, which the first one's dispatch calls (`__cpuidex`,
`_xgetbv`). Every mutable datum the generated C defines (the globals
`gs_globals_main`, the data stacks' counters, channel queues, the init flag,
the thread-local pointers) is emitted through one macro that defines it in
the first unit and declares it `extern` in the second, so both units share
one of each; code and constant data may be duplicated freely. The build
(`src/build.h`) compiles the second unit only for MSVC with a function in
lanes.

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
