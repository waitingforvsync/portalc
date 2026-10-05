#!/usr/bin/env python3
"""refcmp.py - two runs of portalc_refcmp against each other.

    refcmp.py OLD NEW       the pixels wrong against the reference in each,
                            the frames each does better, and the worst
                            regressions and best gains by pose

Both runs must be of the same map and pose set (the same build of the pose
generator), so their records line up frame by frame. A frame either run
skipped (wrong pixels 0xFFFF: its eye in no cell) is left out of both.
"""

import struct
import sys

REC = struct.Struct('<HHBBH')
SKIPPED = 0xFFFF


def load(path):
    with open(path, 'rb') as f:
        data = f.read()
    return [REC.unpack_from(data, i) for i in range(0, len(data), REC.size)]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    old, new = load(sys.argv[1]), load(sys.argv[2])
    if len(old) != len(new) or any(a[:4] != b[:4] for a, b in zip(old, new)):
        sys.exit('refcmp.py: the two runs have different poses')

    pairs = [(a, b) for a, b in zip(old, new) if SKIPPED not in (a[4], b[4])]
    skipped = len(old) - len(pairs)
    old, new = [a for a, _ in pairs], [b for _, b in pairs]
    eo = sum(r[4] for r in old)
    en = sum(r[4] for r in new)
    diffs = [(b[4] - a[4], a[:4], a[4], b[4]) for a, b in zip(old, new)]
    worse = sorted((d for d in diffs if d[0] > 0), reverse=True)
    better = sorted((d for d in diffs if d[0] < 0))
    print(f'{len(old)} frames ({skipped} skipped); pixels wrong: old {eo:,}, new {en:,} ({en - eo:+,})')
    print(f'new worse in {len(worse)} frames, better in {len(better)}')
    for title, rows in (('worst regressions', worse[:10]), ('best gains', better[:5])):
        print(f'{title} (x, y, angle, cell: old -> new):')
        for d, pose, a, b in rows:
            print(f'  {pose[0]:5d} {pose[1]:5d} {pose[2]:3d} {pose[3]:2d}: {a:5d} -> {b:5d}')


if __name__ == '__main__':
    main()
