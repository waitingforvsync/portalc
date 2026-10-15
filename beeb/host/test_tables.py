#!/usr/bin/env python3
"""test_tables.py - boot PORTAL in b2, as a Master 128, and check that the
loader made every table just as arith.c does.

    test_tables.py DISC SYMBOLS TABLES

SYMBOLS is Baron's symbol dump, for where the tables are; TABLES is
portalc_tables' JSON, the C's. trowhi, which the C hasn't (it works the
page out), is checked against T_ORG; shr3, which it writes out as b >> 3,
against b >> 3. The tables are read until they match,
or for 20 seconds: the loader only writes them once.
"""

import json
import sys
import time

import b2

# the Beeb's table, where in it, and the C's, where in that, and how many
CHECKS = [
    ('nrm', 0, 'nrm', 0, 256),
    ('k8', 0, 'k8', 0, 256),
    ('logf', 0, 'logf', 0, 256),
    ('expf', 0, 'expf', 0, 256),
    ('atan', 0, 'atan', 0, 256),
    ('lhyp', 0, 'lhyp', 0, 256),
    ('sinq', 0, 'sinq', 0, 256),
    ('a2c', 0, 'a2c', 0, 256),
    ('lsec16lo', 0, 'lsec', 0, 128),
    ('lsec16lo', 0, 'lsec16lo', 0, 256),
    ('lsec16hi', 0, 'lsec16hi', 0, 256),
    ('sq1lo', 0, 'sq1lo', 0, 512),
    ('sq1hi', 0, 'sq1hi', 0, 512),
    ('sq2lo', 0, 'sq2lo', 0, 512),
    ('sq2hi', 0, 'sq2hi', 0, 512),
    ('rpost', 32, 'rpost', 32, 200 - 32),
    ('trowhi', 0, 'trowhi', 0, 65),
    ('shr3', 0, 'shr3', 0, 256),
]


def symbols(path):
    """Every label and assignment in PORTAL's assembly, by name."""
    with open(path) as f:
        dump = json.load(f)
    found = {}
    for asm in dump['assemblies']:
        if not any(s.endswith('portal.6502') for s in asm['sources']):
            continue
        for sec in asm['sections']:
            for group in ('labels', 'assignments'):
                for name, sym in sec.get(group, {}).items():
                    if isinstance(sym.get('value'), int):
                        found[name] = sym['value']
    return found


def read(syms):
    return [b2.peek(syms[name] + off, n) for name, off, _, _, n in CHECKS]


def main():
    if len(sys.argv) != 4:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    disc, sympath, cpath = sys.argv[1:]
    syms = symbols(sympath)
    with open(cpath) as f:
        want = {k: bytes.fromhex(v) for k, v in json.load(f).items()}
    want['trowhi'] = bytes((syms['T_ORG'] >> 8) + y // 4 for y in range(65))
    want['shr3'] = bytes(b >> 3 for b in range(256))
    expect = [want[c][coff:coff + n] for _, _, c, coff, n in CHECKS]

    proc = None if b2.alive() else b2.private()
    try:
        b2.reset()
        b2.boot(disc)
        end = time.time() + 20
        while True:
            got = read(syms)
            if got == expect or time.time() > end:
                break
            time.sleep(0.5)
    finally:
        b2.stop(proc)

    bad = 0
    for (name, off, c, coff, n), g, w in zip(CHECKS, got, expect):
        if g == w:
            print('  %-9s %3d bytes: the same' % (c, n))
            continue
        bad += 1
        diffs = [i for i in range(n) if g[i] != w[i]]
        i = diffs[0]
        print('  %-9s %3d bytes: %d differ, first [%d] Beeb &%02X, C &%02X'
              % (c, n, len(diffs), coff + i, g[i], w[i]))
    print('tables: %s' % ('all the same as the C' if bad == 0 else '%d differ' % bad))
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
