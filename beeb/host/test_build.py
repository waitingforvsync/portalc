#!/usr/bin/env python3
"""test_build.py - boot make test's PORTAL in b2, as a Master 128, and check its checks' answers
(test/) against the C's (portalc_arithref, portalc_vcacheref, portalc_captureref)."""

import json
import sys
import time

import b2
from test_tables import symbols

USAGE = 'usage: test_build.py DISC SYMBOLS ARITH VCACHE CAPTURE (the test build, its symbols, the C\'s JSON)'
DONE = 0xA5


def fletcher(answers):
    """LOG2_16's sums: each answer's fraction, then its whole part."""
    s1 = s2 = 0
    for v in answers:
        for b in (v & 0xFF, v >> 8):
            s1 = (s1 + b) & 0xFF
            s2 = (s2 + s1) & 0xFF
    return s1, s2


def polar_sums(flat):
    """POLAR's sums, a row of 256 points at a time: each one's angle, then its distance's log."""
    s1s, s2s = [], []
    for row in range(256):
        s1 = s2 = 0
        for i in range(row * 512, row * 512 + 512, 2):
            a, logdist = flat[i], flat[i + 1] & 0xFFFF
            for b in (a & 0xFF, a >> 8, logdist & 0xFF, logdist >> 8):
                s1 = (s1 + b) & 0xFF
                s2 = (s2 + s1) & 0xFF
        s1s.append(s1)
        s2s.append(s2)
    return bytes(s1s), bytes(s2s)


def fletcher_bytes(values):
    """A pair of sums over bytes, as test/vcache.6502's FLETCHER makes them."""
    s1 = s2 = 0
    for b in values:
        s1 = (s1 + b) & 0xFF
        s2 = (s2 + s1) & 0xFF
    return s1, s2


def vcache_expected(c, n):
    """The vertex cache's sums, by the camera's vertex: each vertex's first entry, its entry
    the second time, and its polar form."""
    flat = c['vcache']
    s1s, s2s = [], []
    for p in range(n):
        bs = []
        for i in range(p * n * 4, (p + 1) * n * 4, 4):
            e1, e2, a, logdist = flat[i], flat[i + 1], flat[i + 2], flat[i + 3] & 0xFFFF
            bs += [e1, e2, a & 0xFF, a >> 8, logdist & 0xFF, logdist >> 8]
        s1, s2 = fletcher_bytes(bs)
        s1s.append(s1)
        s2s.append(s2)
    return [
        ('VCACHE_VERT s1', 't_vcache1', 0, 0, bytes(s1s)),
        ('VCACHE_VERT s2', 't_vcache2', 0, 0, bytes(s2s)),
    ]


def capture_expected(c):
    """The root walk's sums, by the camera's cell: at each angle, the side, then the split's low
    byte and high."""
    flat, cells = c['root'], c['cells']
    s1s, s2s = [], []
    for cell in range(1, cells):
        bs = []
        base = (cell - 1) * 512
        for i in range(base, base + 512, 2):
            side, wrap = flat[i], flat[i + 1]
            bs += [side, wrap & 0xFF, wrap >> 8]
        s1, s2 = fletcher_bytes(bs)
        s1s.append(s1)
        s2s.append(s2)
    return [
        ('root_side s1', 't_root1', 1, 1, bytes(s1s)),
        ('root_side s2', 't_root2', 1, 1, bytes(s2s)),
    ]


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
    near1, near2 = polar_sums(c['polar_near'])
    far1, far2 = polar_sums(c['polar_far'])
    return checks + [
        ('EXP2_8 byte', 't_exp', 0, 0, bytes(v & 0xFF for v in c['exp2'])),
        ('EXP2_8 carry', 't_expc', 0, 0, bytes(v >> 8 for v in c['exp2'])),
        ('POLAR bound', 't_bits', 0, 0, bytes([c['polar_bits']])),
        ('POLAR near s1', 't_pnear1', 0, 0, near1),
        ('POLAR near s2', 't_pnear2', 0, 0, near2),
        ('POLAR far s1', 't_pfar1', 0, 0, far1),
        ('POLAR far s2', 't_pfar2', 0, 0, far2),
    ]


def main():
    if len(sys.argv) != 6:
        print(USAGE, file=sys.stderr)
        return 2
    disc, sympath, apath, vpath, cpath = sys.argv[1:]
    syms = symbols(sympath)
    with open(apath) as f:
        checks = expected(json.load(f))
    with open(vpath) as f:
        vc = json.load(f)
    checks += vcache_expected(vc, vc['verts'])
    with open(cpath) as f:
        checks += capture_expected(json.load(f))

    proc = None if b2.alive() else b2.private()
    try:
        b2.reset()
        b2.boot(disc)
        end = time.time() + 120
        while b2.peek(syms['t_done'], 1)[0] != DONE:
            if time.time() > end:
                print('test: the checks never finished')
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
    print('test: %s' % ('all the same as the C' if bad == 0 else '%d differ' % bad))
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
