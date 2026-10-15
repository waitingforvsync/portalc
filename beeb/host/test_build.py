#!/usr/bin/env python3
"""test_build.py - boot one of make test's checks in b2, as a Master 128 (its own PORTAL, from
test/CHECK_portal.6502), and compare its answers with its C tool's: portalc_CHECKref."""

import json
import sys
import time

import b2
from test_tables import symbols

USAGE = ('usage: test_build.py CHECK DISC SYMBOLS JSON (arith, vcache, capture, plot, clip, walk, '
         'walk3 or fill; its build, its symbols, and the C\'s answers)')
DONE = 0xA5


def fletcher_add(s1, s2, b):
    """One byte into a pair of Fletcher sums, mod 255 as the 6502 makes them: a carry out goes
    back in (so 255 stays 255)."""
    s1 += b
    if s1 > 255:
        s1 -= 255
    s2 += s1
    if s2 > 255:
        s2 -= 255
    return s1, s2


def fletcher(answers):
    """LOG2_16's sums: each answer's fraction, then its whole part, mod 255."""
    s1 = s2 = 0
    for v in answers:
        for b in (v & 0xFF, v >> 8):
            s1, s2 = fletcher_add(s1, s2, b)
    return s1, s2


def polar_sums(flat):
    """POLAR's sums, a row of 256 points at a time: each one's angle, then its distance's log."""
    s1s, s2s = [], []
    for row in range(256):
        s1 = s2 = 0
        for i in range(row * 512, row * 512 + 512, 2):
            a, logdist = flat[i], flat[i + 1] & 0xFFFF
            for b in (a & 0xFF, a >> 8, logdist & 0xFF, logdist >> 8):
                s1, s2 = fletcher_add(s1, s2, b)
        s1s.append(s1)
        s2s.append(s2)
    return bytes(s1s), bytes(s2s)


def fletcher_bytes(values):
    """A pair of sums over bytes, as test/vcache.6502's FLETCHER makes them."""
    s1 = s2 = 0
    for b in values:
        s1, s2 = fletcher_add(s1, s2, b)
    return s1, s2


def vcache_expected(c):
    """The vertex cache's sums, by the camera's vertex: each vertex's first entry, its entry
    the second time, and its polar form."""
    flat, n = c['vcache'], c['verts']
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


def line_bytes(f, i):
    """A line's bytes from the C's values at f[i], as LINE_SUMS sums them: its columns, its side
    (&80 below), then if it has columns its row and gradient (low byte first) and end row; and
    where its values end."""
    x0, x1, below = f[i:i + 3]
    bs = [x0, x1, below << 7]
    i += 3
    if x0 < x1:
        y, g, rl = f[i:i + 3]
        bs += [y & 0xFF, y >> 8, g & 0xFF, g >> 8, rl]
        i += 3
    return bs, i


def capture_expected(c):
    """The capture's sums, by the camera's place: at each angle, the root walk (its 16-bit fields
    low byte first), its visit (a height as the log's low byte, then its high with the side in bit
    7), the eye's height, the counts and each byte column's toggle_end; then each side of the eye's
    cell walked, its screen angles and columns, and if it has columns its ends and its lines at the
    cell's ceiling and floor and the eye's height. Then HEIGHT_OF's, by the eye's height: each
    height's low byte, then its high; and the made-up lines', by row."""
    frames, places = c['frames'], c['places']
    s1s, s2s = [], []
    for p in range(places):
        bs = []
        for a in range(256):
            f = frames[p * 256 + a]
            vi, x, tmax, bmin, split_angle, u0, u1, c0, c1 = f[0:9]
            vcell, vside, vxr, vkept, hcb, hcl, hfb, hfl = f[9:17]
            u0 &= 0xFFFF
            u1 &= 0xFFFF
            bs += [vi, x, tmax, bmin, split_angle & 0xFF, split_angle >> 8, u0 & 0xFF, u0 >> 8, u1 & 0xFF, u1 >> 8,
                   c0, c1]
            bs += [vcell, vside, vxr, vkept, hcl & 0xFF, (hcl >> 8) | (hcb << 7), hfl & 0xFF,
                   (hfl >> 8) | (hfb << 7)]
            bs += f[17:21] + f[21:53]
            i = 53
            while i < len(f):
                side, u0, u1, c0, c1 = f[i:i + 5]
                i += 5
                bs += [side, u0 & 0xFF, (u0 >> 8) & 0xFF, u1 & 0xFF, (u1 >> 8) & 0xFF, c0, c1]
                if c0 < c1:
                    for v in f[i:i + 3]:
                        bs += [v & 0xFF, (v >> 8) & 0xFF]
                    i += 3
                    for _ in range(3):
                        lb, i = line_bytes(f, i)
                        bs += lb
        s1, s2 = fletcher_bytes(bs)
        s1s.append(s1)
        s2s.append(s2)
    h1s, h2s = [], []
    h = c['height']
    for eye in range(256):
        bs = []
        for z in range(256):
            below, l = h[(eye * 256 + z) * 2:(eye * 256 + z) * 2 + 2]
            bs += [l & 0xFF, (l >> 8) | (below << 7)]
        s1, s2 = fletcher_bytes(bs)
        h1s.append(s1)
        h2s.append(s2)
    l1s, l2s = [], []
    lines, i = c['lines'], 0
    for row in range(256):
        bs = []
        for _ in range(256):
            lb, i = line_bytes(lines, i)
            bs += lb
        s1, s2 = fletcher_bytes(bs)
        l1s.append(s1)
        l2s.append(s2)
    return [
        ('capture s1', 't_capture1', 0, 0, bytes(s1s)),
        ('capture s2', 't_capture2', 0, 0, bytes(s2s)),
        ('HEIGHT_OF s1', 't_height1', 0, 0, bytes(h1s)),
        ('HEIGHT_OF s2', 't_height2', 0, 0, bytes(h2s)),
        ('lines s1', 't_lines1', 0, 0, bytes(l1s)),
        ('lines s2', 't_lines2', 0, 0, bytes(l2s)),
        ('instances s1', 't_instances1', 0, 0, hex_sums(c['insts'])[0]),
        ('instances s2', 't_instances2', 0, 0, hex_sums(c['insts'])[1]),
        ('deep s1', 't_deep1', 0, 0, hex_sums([c['deep']])[0]),
        ('deep s2', 't_deep2', 0, 0, hex_sums([c['deep']])[1]),
    ]


def hex_sums(hexes):
    """A pair of sums over each of a list of hex strings' bytes, as two byte strings."""
    sums = [fletcher_bytes(bytes.fromhex(h)) for h in hexes]
    return bytes(s[0] for s in sums), bytes(s[1] for s in sums)


def plot_expected(c):
    """The plotters' sums: by gradient, each run's count, record (the last) and toggle_end,
    then T after them, and after the unplot; and T after each four start columns' flat runs."""
    runs1, runs2 = hex_sums(g['runs'] for g in c['runs'])
    plot1, plot2 = hex_sums(g['t'] for g in c['runs'])
    unplot1, unplot2 = hex_sums(g['unplot'] for g in c['runs'])
    flat1, flat2 = hex_sums(c['flat'])
    return [
        ('plot_run s1', 't_runs1', 0, 0, runs1),
        ('plot_run s2', 't_runs2', 0, 0, runs2),
        ('plot_run T s1', 't_plot1', 0, 0, plot1),
        ('plot_run T s2', 't_plot2', 0, 0, plot2),
        ('unplot T s1', 't_unplot1', 0, 0, unplot1),
        ('unplot T s2', 't_unplot2', 0, 0, unplot2),
        ('plot_flat s1', 't_flat1', 0, 0, flat1),
        ('plot_flat s2', 't_flat2', 0, 0, flat2),
    ]


def clip_expected(c):
    """The clips' sums: each line's cases' answers (the extent and room, the records, toggle_end
    and the bounds after), and T after every two lines."""
    cases1, cases2 = hex_sums(c['cases'])
    t1, t2 = hex_sums(c['t'])
    return [
        ('clip cases s1', 't_cases1', 0, 0, cases1),
        ('clip cases s2', 't_cases2', 0, 0, cases2),
        ('clip T s1', 't_t1', 0, 0, t1),
        ('clip T s2', 't_t2', 0, 0, t2),
    ]


def walk_expected(c):
    """The walk's sums, by the camera's place: each frame's counts, records, toggle_end and visits,
    T after the first frame's unplot, and T after the place's other frames."""
    answers1, answers2 = hex_sums(p['answers'] for p in c['places'])
    clear1, clear2 = hex_sums(p['clear'] for p in c['places'])
    t1, t2 = hex_sums(p['t'] for p in c['places'])
    return [
        ('walk s1', 't_answers1', 0, 0, answers1),
        ('walk s2', 't_answers2', 0, 0, answers2),
        ('unplot T s1', 't_clear1', 0, 0, clear1),
        ('unplot T s2', 't_clear2', 0, 0, clear2),
        ('walk T s1', 't_t1', 0, 0, t1),
        ('walk T s2', 't_t2', 0, 0, t2),
    ]


def fill_expected(c):
    """The fill's sums: each made-up frame's screen after it."""
    screen1, screen2 = hex_sums(c['screens'])
    return [
        ('fill s1', 't_screen1', 0, 0, screen1),
        ('fill s2', 't_screen2', 0, 0, screen2),
    ]


def arith_expected(c):
    """The arithmetic macros' answers. Each check's are (name, label, where in it, the first
    answer's input, the answers)."""
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
    exp1, exp2 = [], []
    for row in range(8):
        bs = []
        for v in c['exp2'][row * 256:row * 256 + 256]:
            bs += [v & 0xFF, v >> 8]
        s1, s2 = fletcher_bytes(bs)
        exp1.append(s1)
        exp2.append(s2)
    near1, near2 = polar_sums(c['polar_near'])
    far1, far2 = polar_sums(c['polar_far'])
    return checks + [
        ('EXP2_8 s1', 't_exp1', 0, 0, bytes(exp1)),
        ('EXP2_8 s2', 't_exp2', 0, 0, bytes(exp2)),
        ('POLAR bound', 't_bits', 0, 0, bytes([c['polar_bits']])),
        ('POLAR near s1', 't_pnear1', 0, 0, near1),
        ('POLAR near s2', 't_pnear2', 0, 0, near2),
        ('POLAR far s1', 't_pfar1', 0, 0, far1),
        ('POLAR far s2', 't_pfar2', 0, 0, far2),
    ]


EXPECTED = {'arith': arith_expected, 'vcache': vcache_expected, 'capture': capture_expected,
            'plot': plot_expected, 'clip': clip_expected, 'walk': walk_expected,
            'walk3': walk_expected, 'fill': fill_expected}


def main():
    if len(sys.argv) != 5 or sys.argv[1] not in EXPECTED:
        print(USAGE, file=sys.stderr)
        return 2
    check, disc, sympath, cpath = sys.argv[1:]
    syms = symbols(sympath)
    with open(cpath) as f:
        checks = EXPECTED[check](json.load(f))

    proc = None if b2.alive() else b2.private()
    try:
        b2.reset()
        b2.boot(disc)
        end = time.time() + 120
        while b2.peek(syms['t_done'], 1)[0] != DONE:
            if time.time() > end:
                print('test_%s: the check never finished' % check)
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
    print('test_%s: %s' % (check, 'all the same as the C' if bad == 0 else '%d differ' % bad))
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
