"""Every interleaving of the two-lock queue with a count of receivers signalled and not yet
back, under sequentially consistent atomics. A state no thread can move from, spurious wakes
aside, is a bug unless all that is left is receivers asleep with no message there for them.
Broken forms are there to show that the checker finds what it is looking for.

A sender: lock tail; store tail+1; unlock tail; load sleeping; load signalled; if sleeping is
more, lock head; if sleeping > signalled: signalled += 1, signal; unlock head.
A receiver: lock head; loop { load tail; if head != tail: take, unlock, done.
sleeping++; load tail again; if tail == head: { wait (head unlocked meanwhile);
if signalled: signalled -= 1 }; sleeping--; }.
Each load and store is its own step.

    python qmodel2.py [largest senders+receivers]
"""
import sys
from collections import deque


def initial(senders, receivers):
    threads = tuple([("S", 0, 0)] * senders + [("R", 0, 0)] * receivers)
    return (threads, 0, 0, 0, 0, None, None, frozenset(), frozenset())


def moves(st, v):
    threads, tail, head, sleeping, signalled, tl, hl, waiters, woken = st
    for i, (kind, pc, loc) in enumerate(threads):
        def go(pc2, loc2=None, **kw):
            t = list(threads)
            t[i] = (kind, pc2, loc if loc2 is None else loc2)
            d = dict(tail=tail, head=head, sleeping=sleeping, signalled=signalled, tl=tl, hl=hl, waiters=waiters, woken=woken)
            d.update(kw)
            return (tuple(t), d["tail"], d["head"], d["sleeping"], d["signalled"], d["tl"], d["hl"], d["waiters"], d["woken"])

        def signal(pc2):
            if waiters:
                for w in waiters:
                    yield go(pc2, waiters=waiters - {w}, woken=woken | {w})
            else:
                yield go(pc2)

        if pc == "done":
            continue
        if kind == "S":
            if pc == 0 and tl is None:
                yield go(1, tl=i)
            elif pc == 1:
                yield go(2, tail=tail + 1)
            elif pc == 2:
                yield go(3, tl=None)
            elif pc == 3:
                # The two looks, in the order the design has or the broken form's.
                if v == "signalled read first":
                    yield go(4, loc2=signalled)
                else:
                    yield go(4, loc2=sleeping)
            elif pc == 4:
                if v == "signalled read first":
                    yield go(5 if sleeping > loc else "done")
                else:
                    yield go(5 if loc > signalled else "done")
            elif pc == 5 and hl is None:
                yield go(6, hl=i)
            elif pc == 6:
                # Broken: counts a signal for a receiver already counted.
                yield go(7 if v == "no check under the lock" or sleeping > signalled else 9)
            elif pc == 7:
                yield go(8, signalled=signalled + 1)
            elif pc == 8:
                yield from signal(9)
            elif pc == 9:
                yield go("done", hl=None)
        else:
            if pc == 0 and hl is None:
                yield go(1, hl=i)
            elif pc == 1:
                yield go(2, loc2=tail)
            elif pc == 2:
                yield go(8, head=head + 1) if head != loc else go(3)
            elif pc == 3:
                yield go(4, sleeping=sleeping + 1)
            elif pc == 4:
                yield go(5, loc2=tail)
            elif pc == 5:
                if loc == head:
                    yield go(6, hl=None, waiters=waiters | {i})
                elif v == "a look that finds a message counts":
                    yield go(61)
                else:
                    yield go(7)
            elif pc == 6:
                # Asleep until signalled; then takes the lock again.
                if i in woken and hl is None:
                    yield go(60 if v == "sleeping down first" else 61, hl=i, woken=woken - {i})
            elif pc == 60:
                # Broken: leaves sleeping before signalled, which a sender reads in that order.
                yield go(62, sleeping=sleeping - 1)
            elif pc == 62:
                yield go(1, signalled=max(signalled - 1, 0))
            elif pc == 61:
                yield go(7, signalled=max(signalled - 1, 0))
            elif pc == 7:
                yield go(1, sleeping=sleeping - 1)
            elif pc == 8:
                yield go("done", hl=None)


def spurious(st):
    threads, tail, head, sleeping, signalled, tl, hl, waiters, woken = st
    for w in waiters:
        yield (threads, tail, head, sleeping, signalled, tl, hl, waiters - {w}, woken | {w})


def stuck(st):
    threads, tail, head = st[0], st[1], st[2]
    waiters = st[7]
    left = [i for i, t in enumerate(threads) if t[1] != "done"]
    if not left:
        return False
    return not (all(threads[i][0] == "R" and i in waiters for i in left) and head == tail)


def check(senders, receivers, v):
    start = initial(senders, receivers)
    seen = {start}
    work = deque([start])
    while work:
        st = work.popleft()
        nxt = list(moves(st, v))
        if not nxt and stuck(st):
            return len(seen), st
        if v.endswith("spurious wakes"):
            nxt += list(spurious(st))
        for n in nxt:
            if n not in seen:
                seen.add(n)
                work.append(n)
    return len(seen), None


LARGEST = int(sys.argv[1]) if len(sys.argv) > 1 else 5
SIZES = [(s, r) for s, r in ((1, 1), (2, 1), (1, 2), (2, 2), (3, 2), (2, 3), (3, 3)) if s + r <= LARGEST]
bad = 0
for v, should_fail in (("as designed", False), ("as designed, spurious wakes", False),
                       ("a look that finds a message counts", False),
                       ("sleeping down first", True), ("signalled read first", True),
                       ("no check under the lock", True)):
    caught = 0
    total = 0
    for senders, receivers in SIZES:
        states, st = check(senders, receivers, v)
        total += states
        if st:
            caught += 1
            if not should_fail:
                print(f"   {senders}x{receivers}: {st}")
    print(f"{v:<36} {total:>9} states over {len(SIZES)} sizes, stuck in {caught}", flush=True)
    if should_fail != (caught > 0):
        print(f"   WRONG: expected {'a bug' if should_fail else 'none'} here", flush=True)
        bad += 1
print("problems:", bad)
sys.exit(1 if bad else 0)
