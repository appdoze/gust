"""Optimal sorting networks for 9 and 13 elements, checked by the 0-1 principle: a network sorts every
input if it sorts every input of zeros and ones."""
NET9 = [(0, 3), (1, 7), (2, 5), (4, 8), (0, 7), (2, 4), (3, 8), (5, 6), (0, 2), (1, 3), (4, 5), (7, 8),
        (1, 4), (3, 6), (5, 7), (0, 1), (2, 4), (3, 5), (6, 8), (2, 3), (4, 5), (6, 7), (1, 2), (3, 4), (5, 6)]
NET13 = [(0, 12), (1, 10), (2, 9), (3, 7), (5, 11), (6, 8), (1, 6), (2, 3), (4, 11), (7, 9), (8, 10), (0, 4),
         (1, 2), (3, 6), (7, 8), (9, 10), (11, 12), (4, 6), (5, 9), (8, 11), (10, 12), (0, 5), (3, 8), (4, 7),
         (6, 11), (9, 10), (0, 1), (2, 5), (6, 9), (7, 8), (10, 11), (1, 3), (2, 4), (5, 6), (9, 10), (1, 2),
         (3, 4), (5, 7), (6, 8), (2, 3), (4, 5), (6, 7), (8, 9), (3, 4), (5, 6)]


def sorts_all(net, n):
    """Whether the network sorts all 2^n inputs of zeros and ones; and the first it does not, if any."""
    for bits in range(1 << n):
        v = [(bits >> i) & 1 for i in range(n)]
        for a, b in net:
            if v[b] < v[a]:
                v[a], v[b] = v[b], v[a]
        if any(v[i] > v[i + 1] for i in range(n - 1)):
            return False, bits
    return True, None


if __name__ == "__main__":
    for name, net, n in (("9", NET9, 9), ("13", NET13, 13)):
        ok, bad = sorts_all(net, n)
        print(f"network {name}: {len(net)} steps, indexes in range {all(0 <= a < b < n for a, b in net)}, sorts every input: {ok}", bad if bad is not None else "")
