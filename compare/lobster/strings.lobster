// String building, splitting and string-keyed counting, see ../EXTRA_SPEC.md section 8.

import std
import dictionary
import .rnd

private let ROUNDS = 10
private let NWORDS = 200000
private let VOCAB = 5000

def bench_strings() -> float:
    var sum = 0
    for(ROUNDS) round:
        let rnd = Rnd { 2024 + round }
        let parts = []::string
        parts.vector_capacity(NWORDS)
        for(NWORDS):
            parts.push("w{rnd.below(VOCAB)}")
        let text = concat_string(parts, " ")
        // 5003 is the prime nearest VOCAB, the bucket count Lobster's dictionary asks for.
        let counts = dictionary<string, int>(5003)
        for(tokenize(text, " ", "")) w:
            counts.add(w, 1)
        var distinct = 0
        var sq = 0
        for(VOCAB) v:
            let c = counts.get("w{v}", 0)
            if c > 0:
                distinct++
                sq += c * c
        sum += text.length + distinct * 1000 + sq
    return float(sum)
