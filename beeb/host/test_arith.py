#!/usr/bin/env python3
"""test_arith.py - boot make test's PORTAL in b2, as a Master 128, and check its arithmetic
macros' answers (test/arith.6502) against the C's (portalc_arithref)."""

import json
import sys
import time

import b2
from test_tables import symbols

USAGE = 'usage: test_arith.py DISC SYMBOLS ARITH (the test build, its symbols, the C\'s JSON)'
DONE = 0xA5


def fletcher(answers):
    """LOG2_16's sums: each answer's fraction, then its whole part."""
    s1 = s2 = 0
    for v in answers:
        for b in (v & 0xFF, v >> 8):
            s1 = (s1 + b) & 0xFF
            s2 = (s2 + s1) & 0xFF
    return s1, s2


def expected(c):
    """(name, label, where in it, the first answer's input, the answers)."""
    log8 = c['log2_8']
    sums = [fletcher(c['log2'][hi * 256 + (1 if hi == 0 else 0):hi * 256 + 256])
            for hi in range(256)]
    checks = [
        ('LOG2_8 fraction', 't_log8f', 1, 1, bytes(v & 0xFF for v in log8[1:])),
        ('LOG2_8 whole', 't_log8w', 1, 1, bytes(v >> 8 for v in log8[1:])),
    ]
    for bits in range(9, 17):
        rows = 1 << (bits - 8)
        checks += [
            ('LOG2_16 %d s1' % bits, 't_log16s1', rows - 2, 0, bytes(s[0] for s in sums[:rows])),
            ('LOG2_16 %d s2' % bits, 't_log16s2', rows - 2, 0, bytes(s[1] for s in sums[:rows])),
        ]
    return checks + [
        ('EXP2_8 byte', 't_exp', 0, 0, bytes(v & 0xFF for v in c['exp2'])),
        ('EXP2_8 carry', 't_expc', 0, 0, bytes(v >> 8 for v in c['exp2'])),
    ]


def main():
    if len(sys.argv) != 4:
        print(USAGE, file=sys.stderr)
        return 2
    disc, sympath, cpath = sys.argv[1:]
    syms = symbols(sympath)
    with open(cpath) as f:
        checks = expected(json.load(f))

    proc = None if b2.alive() else b2.private()
    try:
        b2.reset()
        b2.boot(disc)
        end = time.time() + 30
        while b2.peek(syms['t_done'], 1)[0] != DONE:
            if time.time() > end:
                print('arith: the check never finished')
                return 1
            time.sleep(0.5)
        got = [b2.peek(syms[label] + at, len(want)) for _, label, at, _, want in checks]
    finally:
        b2.stop(proc)

    bad = 0
    for (name, _, _, first, want), g in zip(checks, got):
        if g == want:
            print('  %-16s %4d answers: the same' % (name, len(want)))
            continue
        bad += 1
        diffs = [i for i in range(len(want)) if g[i] != want[i]]
        i = diffs[0]
        print('  %-16s %4d answers: %d differ, first [%d] Beeb &%02X, C &%02X'
              % (name, len(want), len(diffs), first + i, g[i], want[i]))
    print('arith: %s' % ('all the same as the C' if bad == 0 else '%d differ' % bad))
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
