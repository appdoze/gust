// Integer-keyed hash map, see ../EXTRA_SPEC.md section 7.

import std
import dictionary
import .rnd

private let ROUNDS = 5
private let N_OPS = 1000000
private let KEYS = 131072

def bench_hashmap() -> float:
    var sum = 0
    for(ROUNDS) round:
        let rnd = Rnd { 31337 + round }
        // Lobster's dictionary has a fixed number of chained buckets, given at construction;
        // 131071 is the prime nearest KEYS.
        let counts = dictionary<int, int>(131071)
        for(N_OPS):
            counts.add(rnd.below(KEYS), 1)
        var found = 0
        var misses = 0
        for(N_OPS):
            // Every stored count is at least 1, so -1 means absent.
            let v = counts.get(rnd.below(2 * KEYS), -1)
            if v >= 0:
                found += v
            else:
                misses++
        sum += counts.get_keys().length * 1000003 + found * 3 + misses
    return float(sum)
