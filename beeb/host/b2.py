#!/usr/bin/env python3
"""b2.py - drive b2 over its HTTP API for the port's checks (PLAN.md 5.1-5.2).

The API (b2's doc/Debug-version.md, "HTTP API"): the server always runs,
on 127.0.0.1:48075; /peek/WIN/BEGIN/+SIZE returns raw bytes, /poke/WIN/ADDR
writes the request body, /reset/WIN?config=NAME resets into a config, and
/run/WIN?name=FILE.ssd mounts the body as drive 0 and boots it with SHIFT
held. Every positional part of a URL is always supplied (a missing one is
undefined behaviour in b2's parser), and spaces in a query are %20.

Test builds talk through the mailbox at &2F0-&2FF (src/layout.6502).

    b2.py alive              exit 0 if b2 answers
    b2.py waitup SECONDS     wait for b2 to answer
    b2.py boot DISC.ssd      reset into the Model B config and boot a disc
    b2.py phase0             the Phase 0 checks (PLAN.md section 6)
    b2.py phase1             the Phase 1 checks: S1, the tables and the map
"""

import json
import os
import statistics
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

BASE = 'http://127.0.0.1:48075'
WIN = 'b2'
CONFIG = 'B/Acorn 1770'
OS_ROM = '/snap/b2/current/assets/roms/OS12.ROM'

HERE = os.path.dirname(os.path.abspath(__file__))
BEEB = os.path.dirname(HERE)
BUILD = os.path.join(BEEB, 'build')

MB = 0x2F0
MB_MAGIC, MB_CMD, MB_ARG, MB_SEQ, MB_ACK, MB_ERR, MB_USECS = (
    MB + 0, MB + 3, MB + 4, MB + 8, MB + 9, MB + 10, MB + 11)
MAGIC = b'PC1'
CMD_PING, CMD_TIME = 0, 1
VSYNC_US = 19968                   # 312 lines of 64us


class B2Error(Exception):
    pass


def request(path, body=None, ctype=None, timeout=30):
    req = urllib.request.Request(BASE + path, data=body, method='POST' if body is not None else 'GET')
    if ctype:
        req.add_header('Content-Type', ctype)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        raise B2Error('%s: HTTP %d %s' % (path, e.code, e.read().decode(errors='replace').strip()))


def alive():
    try:
        request('/peek/%s/0/+1' % WIN, timeout=2)
        return True
    except (OSError, B2Error):
        return False


def peek(addr, n):
    data = request('/peek/%s/%x/+%d' % (WIN, addr, n))
    if len(data) != n:
        raise B2Error('peek &%X+%d returned %d bytes' % (addr, n, len(data)))
    return data


def poke(addr, data):
    request('/poke/%s/%x' % (WIN, addr), body=bytes(data), ctype='application/octet-stream')


def reset(config=CONFIG):
    request('/reset/%s?config=%s' % (WIN, urllib.parse.quote(config, safe='')))


def run_disc(path):
    with open(path, 'rb') as f:
        image = f.read()
    request('/run/%s?name=%s' % (WIN, urllib.parse.quote(os.path.basename(path))),
            body=image, ctype='application/x.acorn.disc-image.ssd')


def check_rom():
    """The machine really is a stock Model B: its OS matches OS 1.20."""
    with open(OS_ROM, 'rb') as f:
        want = f.read()[:0x3C00]
    got = peek(0xC000, 0x3C00)
    if got != want:
        diffs = sum(1 for a, b in zip(got, want) if a != b)
        raise B2Error('the ROM at &C000-&FBFF is not OS 1.20 (%d bytes differ): wrong config?' % diffs)


def dump_failure(reason, syms=None):
    stamp = time.strftime('%Y%m%d-%H%M%S')
    d = os.path.join(BUILD, 'fail-' + stamp)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, 'reason.txt'), 'w') as f:
        f.write(reason + '\n')
    try:
        for name, (a, n) in {'mode7': (0x7C00, 0x3E8), 'pages0-2': (0, 0x300),
                             'mailbox': (MB, 16)}.items():
            with open(os.path.join(d, name + '.bin'), 'wb') as f:
                f.write(peek(a, n))
        m7 = peek(0x7C00, 0x3E8)
        with open(os.path.join(d, 'mode7.txt'), 'w') as f:
            for row in range(25):
                f.write(''.join(chr(c & 0x7F) if 32 <= (c & 0x7F) < 127 else ' '
                                for c in m7[row * 40:row * 40 + 40]).rstrip() + '\n')
    except (OSError, B2Error) as e:
        with open(os.path.join(d, 'reason.txt'), 'a') as f:
            f.write('dump incomplete: %s\n' % e)
    return d


def boot(path, magic=True, timeout=60):
    """Reset into the stock Model B, check its OS, boot a disc; for a test
    build, wait for the mailbox's magic."""
    reset()
    check_rom()
    run_disc(path)
    if not magic:
        return
    t = time.time()
    while time.time() - t < timeout:
        if peek(MB_MAGIC, 3) == MAGIC:
            return
        time.sleep(0.05)
    d = dump_failure('no magic %ds after booting %s' % (timeout, path))
    raise B2Error('no magic after booting %s (dump in %s)' % (path, d))


class Mailbox:
    def __init__(self):
        self.seq = peek(MB_ACK, 1)[0]

    def call(self, cmd, args=(0, 0, 0, 0), timeout=60):
        """Run a command; returns (err, elapsed microseconds)."""
        self.seq = (self.seq % 255) + 1              # never 0: start zeroes ack
        a = list(args) + [0] * (4 - len(args))
        poke(MB_CMD, [cmd] + a + [self.seq])         # cmd, args, seq: one request
        t = time.time()
        while time.time() - t < timeout:
            m = peek(MB, 16)
            if m[MB_ACK - MB] == self.seq:
                err = m[MB_ERR - MB]
                us = int.from_bytes(m[MB_USECS - MB:MB_USECS - MB + 4], 'little')
                return err, us
            time.sleep(0.002)
        d = dump_failure('command %d (seq %d) timed out after %ds' % (cmd, self.seq, timeout))
        raise B2Error('command %d timed out (dump in %s)' % (cmd, d))


def symbols(build):
    with open(os.path.join(BUILD, build + '.json')) as f:
        d = json.load(f)
    out = {}
    for syms in d.values():
        out.update(syms)
    return out


def check_image(build, progname):
    """S0: the running image at &300 is the file's payload, byte for byte."""
    syms = symbols(build)
    n = syms['entry'] - syms['payload']
    with open(os.path.join(BUILD, build, progname), 'rb') as f:
        want = f.read()[:n]
    got = peek(0x300, n)
    bad = [i for i in range(n) if got[i] != want[i]]
    return n, bad


def phase0():
    ok = True

    def result(name, good, detail):
        nonlocal ok
        ok = ok and good
        print('%-6s %s: %s' % ('ok' if good else 'FAIL', name, detail))

    for tb in (0, 1):
        build = 'harness%d' % tb
        t = time.time()
        boot(os.path.join(BUILD, build + '.ssd'))
        result('boot', True, '%s up in %.1fs (reset, OS 1.20 checked, disc booted, magic seen)'
               % (build, time.time() - t))
        n, bad = check_image(build, 'HARNESS')
        result('S0', not bad, '%s: &%X bytes at &300 %s' % (
            build, n, 'match the file' if not bad else '%d differ, first at &%X' % (len(bad), 0x300 + bad[0])))
        mb = Mailbox()

        rtt = []
        for _ in range(100):
            t = time.perf_counter()
            err, _ = mb.call(CMD_PING)
            rtt.append((time.perf_counter() - t) * 1000)
            if err:
                break
        result('ping', err == 0, 'HTTP round trip: median %.1f ms, max %.1f ms over %d commands'
               % (statistics.median(rtt), max(rtt), len(rtt)))

        err, us = mb.call(CMD_TIME, (50, 0))
        want = 50 * VSYNC_US
        result('irq', err == 0 and abs(us - want) <= 50,
               '50 vsyncs took %d us (%.2f us each; a Model B frame is %d)' % (us, us / 50, VSYNC_US))

    # the soak: 5 emulated minutes of vsyncs, the image intact (a stray NMI
    # lands on the RTI in nrm's entry 0 at &D00)
    t = time.time()
    err, us = mb.call(CMD_TIME, (15000 & 255, 15000 >> 8), timeout=600)
    n, bad = check_image('harness1', 'HARNESS')
    result('soak', err == 0 and not bad and abs(us - 15000 * VSYNC_US) <= 100,
           '15000 vsyncs (%.1f emulated minutes) in %.0fs: %d us, image %s'
           % (us / 60e6, time.time() - t, us, 'intact' if not bad else 'changed'))

    # the release build: boots, and the IRQ swaps the buffers
    boot(os.path.join(BUILD, 'portal.ssd'), magic=False)
    syms = symbols('portal')
    time.sleep(2)
    s0 = peek(syms['swapstamp'], 1)[0]
    f0 = peek(syms['mainloop.frame'] if 'mainloop.frame' in syms else syms['frame'], 1)[0]
    time.sleep(1)
    s1 = peek(syms['swapstamp'], 1)[0]
    n, bad = check_image('portal', 'PORTAL')
    result('release', s1 != s0 and not bad, 'PORTAL running: swapstamp %d -> %d, image &%X bytes %s'
           % (s0, s1, n, 'intact' if not bad else 'changed'))
    print('phase 0:', 'all checks pass' if ok else 'FAILURES')
    return ok


def check_tables():
    """S1: &300-&EFF holds arith.c's tables as the Beeb lays them out."""
    with open(os.path.join(BUILD, 'expect', 'tables.bin'), 'rb') as f:
        want = f.read()
    got = peek(0x300, len(want))
    return len(want), [i for i in range(len(want)) if got[i] != want[i]]


def check_map(build, which):
    """The map arrays at their labels hold what the C's map holds."""
    syms = symbols(build)
    with open(os.path.join(BUILD, 'expect', 'map%d.json' % which)) as f:
        want = json.load(f)
    bad = []
    total = 0
    for label, values in want.items():
        got = peek(syms[label], len(values))
        total += len(values)
        bad += ['%s[%d]' % (label, i) for i in range(len(values)) if got[i] != values[i]]
    return len(want), total, bad


def phase1():
    ok = True

    def result(name, good, detail):
        nonlocal ok
        ok = ok and good
        print('%-6s %s: %s' % ('ok' if good else 'FAIL', name, detail))

    for build, which in (('harness0', 0), ('harness1', 0), ('harness0_map7', 7), ('harness0_map9', 9)):
        boot(os.path.join(BUILD, build + '.ssd'))
        n, bad = check_image(build, 'HARNESS')
        result('S0', not bad, '%s: &%X bytes at &300 %s' % (
            build, n, 'match the file' if not bad else '%d differ, first at &%X' % (len(bad), 0x300 + bad[0])))
        n, bad = check_tables()
        result('S1', not bad, '%s: tables &300-&%X %s' % (
            build, 0x300 + n - 1, 'match arith.c' if not bad else '%d differ, first at &%X' % (len(bad), 0x300 + bad[0])))
        arrays, total, bad = check_map(build, which)
        result('map', not bad, '%s: map %d, %d arrays (%d bytes) %s' % (
            build, which, arrays, total, 'match the C' if not bad else '%d differ: %s' % (len(bad), bad[:5])))
    print('phase 1:', 'all checks pass' if ok else 'FAILURES')
    return ok


S2_NAMES = ['mulu8', 'mulu16_frac', 'mulu16_frac_round', 'mulg_u16', 'recip8', 'recip16', 'mul_recip8',
            'mul_recip16', 'div_t', 'div_cols', 'div_q8', 'grad_mag', 'gradq8_mag', 'project_col',
            'project_col_q8', 'project_q8', 'project_rowd', 'lerp16', 'rot', 'dir8', 'ray_basis']
CMD_S2 = 3
TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build', 'beeb', 'host', 'portalc_beebtool')


def s2_expect(fn, rnd, start, count):
    """The C's answer (beebtool s2): crc, calls, last outputs, final state."""
    out = subprocess.run([TOOL, 's2', str(fn), str(rnd), str(start), str(count)],
                         capture_output=True, text=True, check=True).stdout.split()
    v = [int(x) for x in out]
    return v[0], v[1], v[2:6], v[6]


def s2_beeb(mb, syms, fn, mode, start, count, timeout=900):
    """The Beeb's answer: crc, calls, last outputs, microseconds."""
    poke(syms['s2_fn'], [fn, mode] + list(start.to_bytes(4, 'little')) + list(count.to_bytes(4, 'little')))
    err, us = mb.call(CMD_S2, timeout=timeout)
    if err:
        raise B2Error('S2 %s: error %d' % (S2_NAMES[fn], err))
    calls = int.from_bytes(peek(syms['s2_calls'], 4), 'little')
    out = peek(syms['s2_out'], 2)
    last = list(peek(syms['s2_last'], 4))
    return out[0] | out[1] << 8, calls, last, us


def s2_find(mb, syms, fn, rnd, start, count):
    """Bisect a mismatching run down to its first bad case."""
    while count > 1:
        half = count // 2
        crc, calls, _, state = s2_expect(fn, rnd, start, half)
        bcrc, bcalls, _, _ = s2_beeb(mb, syms, fn, rnd, start, half)
        if (bcrc, bcalls) == (crc, calls):
            start, count = state, count - half
        else:
            count = half
    crc, calls, last, _ = s2_expect(fn, rnd, start, 1)
    _, bcalls, blast, _ = s2_beeb(mb, syms, fn, rnd, start, 1)
    return start, last, blast


# The runs: (routine, random, start or seed, cases, timed). Exhaustive where
# the domain allows; a million random cases otherwise, plus edge runs: the
# counter sweeps the low 16 bits through every value while the high half
# holds an edge (a denominator of 255, 256 or 257, a depth of 31 or 32...).
def _edges(fn, his):
    return [(fn, 0, h << 16, 65536, False) for h in his]


S2_RUNS = [
    (0, 0, 0, 65536, True), (4, 0, 0, 256, True), (5, 0, 0, 65536, True), (6, 0, 0, 65536, True),
    (1, 1, 1, 1000000, True), (2, 1, 2, 1000000, True), (3, 1, 3, 1000000, True), (7, 1, 7, 1000000, True),
    (8, 1, 8, 1000000, True), (9, 1, 9, 1000000, True), (10, 1, 10, 1000000, True),
    (11, 0, 0, 65536, True), (12, 0, 0, 65 << 16, True),
    (13, 1, 13, 1000000, True), (14, 1, 14, 1000000, True), (15, 1, 15, 1000000, True), (16, 1, 16, 1000000, True),
    (17, 1, 17, 1000000, True), (18, 0, 0, 1 << 20, True), (18, 1, 18, 1000000, True), (19, 0, 0, 65536, True),
    (20, 0, 0, 256, True),
] + _edges(1, [0, 1, 127, 128, 255]) + _edges(2, [0, 1, 127, 128, 255]) + _edges(3, [0, 1, 127, 128, 255]) \
  + _edges(7, [0, 1, 127, 128, 255]) \
  + _edges(8, [1, 2, 127, 128, 129, 255, 256, 257, 4095, 4096, 32768, 65535]) \
  + _edges(9, [1, 2, 127, 128, 129, 255, 256, 257, 4095, 4096, 32768, 65535]) \
  + _edges(10, [0x100, 0x101, 0x17F, 0x180, 0x1FF, 0x8000, 0xFFFF]) \
  + _edges(13, [1, 2, 31, 32, 33, 255, 256, 4095, 65535]) \
  + _edges(14, [1, 2, 16, 255, 256, 16383]) \
  + _edges(15, [1, 2, 31, 32, 63, 64, 255, 256, 16383, 16384, 65535]) \
  + _edges(16, [0, 1, 0x0F, 0x10, 0xFF])


def phase2(only=None, scale=1.0):
    """Check S2: every arithmetic routine against the C, and its cost."""
    build = 'harness0'
    boot(os.path.join(BUILD, build + '.ssd'))
    syms = symbols(build)
    mb = Mailbox()
    ok = True
    for fn, rnd, start, count, timed in S2_RUNS:
        if only is not None and S2_NAMES[fn] not in only:
            continue
        count = max(1, int(count * scale))
        crc, calls, _, _ = s2_expect(fn, rnd, start, count)
        bcrc, bcalls, _, us = s2_beeb(mb, syms, fn, rnd, start, count)
        good = (bcrc, bcalls) == (crc, calls)
        kind = 'random' if rnd else ('edge &%04X' % (start >> 16) if start >= 1 << 16 else 'all')
        what = '%s %s, %d cases' % (kind, S2_NAMES[fn], calls)
        if good and timed:
            _, _, _, base = s2_beeb(mb, syms, fn, rnd | 0x80, start, count)
            print('ok     S2 %-44s %7.1f cycles a call' % (what, (us - base) * 2.0 / max(1, calls)))
        elif good:
            print('ok     S2 %s' % what)
        else:
            ok = False
            bad, want, got = s2_find(mb, syms, fn, rnd, start, count)
            print('FAIL   S2 %-40s first bad case at %s %d: C %s, Beeb %s'
                  % (what, 'seed' if rnd else 'counter', bad, want, got))
    print('phase 2:', 'all checks pass' if ok else 'FAILURES')
    return ok


def spike(seeds=range(256)):
    """The recursion spike (test/spike.6502) against host/spike.py's model, seed by seed."""
    import spike as model
    build = 'harness0'
    boot(os.path.join(BUILD, build + '.ssd'))
    syms = symbols(build)
    out = syms['spike_out']
    mb = Mailbox()
    bad = 0
    lows = 0xFF
    total_nodes = 0
    total_us = 0
    depths = set()
    for seed in seeds:
        err, _ = mb.call(2, (seed,))
        r = peek(out, 13)
        got = {'sum': r[0] | r[1] << 8, 'nodes': r[2] | r[3] << 8, 'closes': r[4] | r[5] << 8,
               'dhigh': r[6]}
        want = model.spike(seed)
        if err or got != want or r[8]:
            bad += 1
            if bad <= 5:
                print('FAIL   seed %d: err %d, got %s, want %s, canary %d' % (seed, err, got, want, r[8]))
        lows = min(lows, r[7])
        depths.add(got['dhigh'])
        total_nodes += got['nodes']
        total_us += int.from_bytes(bytes(r[9:13]), 'little')
    n = len(seeds)
    print('%-6s spike: %d seeds, %d portals walked, deepest level %d..%d: %s'
          % ('ok' if not bad else 'FAIL', n, total_nodes, min(depths), max(depths),
             'all match the model' if not bad else '%d differ' % bad))
    print('       lowest S at the bottom of the chain &%02X: %d bytes of hardware stack, %d with an IRQ on top'
          % (lows, 0xFF - lows, 0xFF - lows + 3))
    print('       %.1f us a portal (dummy chain, IRQ on)' % (total_us / total_nodes))
    return not bad


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == 'alive':
        return 0 if alive() else 1
    if cmd == 'waitup':
        limit = float(argv[2]) if len(argv) > 2 else 30
        t = time.time()
        while time.time() - t < limit:
            if alive():
                return 0
            time.sleep(0.25)
        print('b2 did not answer within %gs' % limit, file=sys.stderr)
        return 1
    if cmd == 'boot':
        boot(argv[2], magic=False)
        return 0
    if cmd == 'phase0':
        return 0 if phase0() else 1
    if cmd == 'phase1':
        return 0 if phase1() else 1
    if cmd == 'spike':
        return 0 if spike() else 1
    if cmd == 'phase2':
        only = set(argv[2].split(',')) if len(argv) > 2 and argv[2] != 'all' else None
        scale = float(argv[3]) if len(argv) > 3 else 1.0
        return 0 if phase2(only, scale) else 1
    print('unknown command %s' % cmd, file=sys.stderr)
    return 2


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv))
    except B2Error as e:
        print('b2: %s' % e, file=sys.stderr)
        sys.exit(1)
