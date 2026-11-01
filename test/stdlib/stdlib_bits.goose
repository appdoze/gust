// Gust: std's popcount, clz and ctz against counting the bits one at a time,
// for zero, each single bit, each run of ones from either end and its
// complement, each pair of bits and random words of several densities.
// Under gust run and --mir the three are the runtime's arithmetic without a
// branch (src/runtime/runtime_ext.h), in a built program the C compiler's
// instructions: the sums printed are the same either way.
import std;

fn ones(x: u64) -> i64 {
    var n = 0;
    for i in 64 { if (x >> i) & 1 != 0 { n += 1; } }
    return n;
}

fn leading(x: u64) -> i64 {
    var n = 0;
    while n < 64 && (x >> (63 - n)) & 1 == 0 { n += 1; }
    return n;
}

fn trailing(x: u64) -> i64 {
    var n = 0;
    while n < 64 && (x >> n) & 1 == 0 { n += 1; }
    return n;
}

struct tally { values: i64, wrong: i64, pop: i64, lead: i64, trail: i64 }

fn check(t: tally&, x: u64) {
    let p = popcount(x);
    let l = clz(x);
    let z = ctz(x);
    if p != ones(x) || l != leading(x) || z != trailing(x) {
        if t.wrong < 5 {
            print("  ", x, ": popcount ", p, " clz ", l, " ctz ", z, ", counted ", ones(x), " ",
                  leading(x), " ", trailing(x));
        }
        t.wrong += 1;
    }
    t.values += 1;
    t.pop += p;
    t.lead += l;
    t.trail += z;
}

fn report(name: u8[:], t: tally) {
    print(name, ": ", t.values, " values, ", t.wrong, " wrong, sums ", t.pop, " ", t.lead, " ",
          t.trail);
}

fn main() {
    let all: u64 = 0xffffffffffffffff;
    var ends = tally { 0, 0, 0, 0, 0 };
    check(ends, 0);
    check(ends, all);
    report("zero and all ones", ends);
    print(popcount(0), " ", clz(0), " ", ctz(0), " ", popcount(all), " ", clz(all), " ", ctz(all));

    var bits = tally { 0, 0, 0, 0, 0 };
    var runs = tally { 0, 0, 0, 0, 0 };
    var pairs = tally { 0, 0, 0, 0, 0 };
    for i in 64 {
        let b: u64 = 1 << i;
        check(bits, b);
        check(runs, b - 1);
        check(runs, ~(b - 1));
        check(runs, ~b);
        check(runs, (b - 1) >> 1 | 0x8000000000000000);
        for j in 64 { check(pairs, b | (1 << j)); }
    }
    report("single bits", bits);
    report("runs of ones", runs);
    report("pairs of bits", pairs);

    var r = rng { 2026 };
    var random = tally { 0, 0, 0, 0, 0 };
    for n in 20000 {
        let a = r.rand_u64();
        let b = r.rand_u64();
        let c = r.rand_u64();
        check(random, a);
        check(random, a & b & c);
        check(random, a | b);
        check(random, a >> (b % 64));
        check(random, a << (c % 64));
        check(random, (a & b & c) >> (r.rand_u64() % 64));
    }
    report("random words", random);
}
