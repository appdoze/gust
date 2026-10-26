# Extra benchmarks

Benchmarks 1 to 5 (`nbody raytrace interp sort astar`) are the ones in
`lobster/tests/compare/SPEC.md`, unchanged. That suite says itself that it
leaves out the areas Lobster is expected to lose (allocation, hash maps,
string building). These three add them, under the same ground rules: same
algorithm and order in every language, idiomatic data structures, in-process
timing (best of `repeats`), no output in the timed region, and the same
`bench<TAB>name<TAB>seconds<TAB>checksum` output line.

The pseudo-random generator is the shared 32-bit LCG from SPEC.md
(`next`, `nextf`, `range`, `below`).

## 6. bintrees

The Benchmarks Game binary-trees, single-threaded. Exercises allocating and
freeing many small objects.

- `MIN_DEPTH = 4`, `MAX_DEPTH = 16`.
- A node is a leaf or has exactly two children. `build(0)` is a leaf,
  `build(d)` is a node with children `build(d - 1)` and `build(d - 1)`.
  `check(leaf) = 1`, `check(node) = 1 + check(left) + check(right)`.
- Order: build, check and drop a stretch tree of depth `MAX_DEPTH + 1`;
  build a long-lived tree of depth `MAX_DEPTH` and keep it; then for
  `d = MIN_DEPTH, MIN_DEPTH + 2, ... MAX_DEPTH`, build, check and drop
  `1 << (MAX_DEPTH - d + MIN_DEPTH)` trees of depth `d`, one at a time;
  finally check the long-lived tree.
- Each tree is its own allocation(s) and is released before the next is
  built. Each language uses its natural way to hold a tree: `Box` in Rust,
  class instances in Lobster, a per-tree pool in Goose.
- Checksum: the sum of every `check` result, `14985902`.

## 7. hashmap

Integer-keyed hash map: inserts, updates, hits and misses.

- `ROUNDS = 5`, round `r` uses seed `31337 + r`, `N_OPS = 1000000`,
  `KEYS = 131072`.
- Each round makes a fresh map from integer to integer, pre-sized for `KEYS`
  entries in the language's own way (Rust `with_capacity`, Goose `reserve`,
  Lobster's bucket-count constructor).
- Phase 1, `N_OPS` times: `k = below(KEYS)`, then `map[k] += 1`, inserting
  `1` when `k` is absent.
- Phase 2, `N_OPS` times: `k = below(2 * KEYS)`; if `k` is present add its
  value to `found`, otherwise add one to `misses`.
- Round checksum: `distinct * 1000003 + found * 3 + misses`, where
  `distinct` is the number of keys in the map. Checksum: the sum over rounds.
- The map is the language's standard one: `std::collections::HashMap`
  (default SipHash) in Rust, `dictionary` from the standard library in
  Goose and in Lobster.

## 8. strings

Formatting integers into a growing string, splitting it, and counting the
pieces in a string-keyed map.

- `ROUNDS = 10`, round `r` uses seed `2024 + r`, `NWORDS = 200000`,
  `VOCAB = 5000`.
- Build `text`: `NWORDS` words separated by single spaces, word `i` being
  `"w"` followed by the decimal digits of `below(VOCAB)`.
- Split `text` on spaces and count every word in a map from string to
  integer, pre-sized for `VOCAB` entries.
- Then for `v` in `0 .. VOCAB - 1`, look up the key `"w"` + decimal `v`;
  where its count `c` is above zero, add one to `distinct` and `c * c` to
  `sq`.
- Round checksum: `len(text) + distinct * 1000 + sq`. Checksum: the sum
  over rounds.
