#!/usr/bin/env python3
"""profile.py - where the frames' cycles go (PLAN.md 5.9, Phase 7's pass).

    profile.py           sample the PC every ~2,000 cycles through whole frames
                         (clear, capture, plot, sprites, fill) over a fixed set
                         of golden poses, and report by routine, by 64-byte
                         window and by instruction; build/profile.txt has the
                         hot listing
    profile.py bench     the same poses unsampled: the cycles they take, by
                         stage (the before-and-after number for a change)

The profiler's builds are SUITE 4 (test/s6.6502's frames, hw.6502's T1
sampler): harness_prof (map 0) and harness_prof_map7 and _map9. The code
sits at the same addresses in all three (the map data follows it).
"""

import collections
import os
import re
import statistics
import sys

import b2

# each set: its build, its poses, every step'th, and how many frames a command
# (few enough that the sample buffer doesn't fill: map 7's sprites are slow)
SETS = (('harness_prof', 'sweep_poses.bin', 1, 4), ('harness_prof', 'rim_poses.bin', 4, 4),
        ('harness_prof_map7', 'map7_poses.bin', 1, 1), ('harness_prof_map9', 'stress_poses.bin', 4, 4))


def poses(name, step):
    with open(os.path.join(b2.BUILD, 'golden', name), 'rb') as f:
        data = f.read()
    return [data[i:i + 6] for i in range(0, len(data), 6)][::step]


def run(sample):
    """Every set's poses through CMD_S6, sprites on: (PC counter, per-frame
    (clear, capture, render) cycles)."""
    pcs = collections.Counter()
    frames = []
    for build, name, step, batch in SETS:
        b2.boot(os.path.join(b2.BUILD, build + '.ssd'))
        syms = b2.symbols(build)
        mb = b2.Mailbox()
        ps = poses(name, step)
        full = 0
        for i in range(0, len(ps), batch):
            some = ps[i:i + batch]
            b2.poke(syms['io_area'], b''.join(some))
            err, _ = mb.call(b2.CMD_S6, (len(some), 1, 0, int(sample)), timeout=600)
            if err:
                raise b2.B2Error('profile: error %d, %s from pose %d' % (err, name, i * step))
            r = b2.peek(syms['s6_out'], b2.S6_REC * len(some))
            for k in range(len(some)):
                g = r[k * b2.S6_REC:(k + 1) * b2.S6_REC]
                frames.append((2 * (g[8] | g[9] << 8), 2 * int.from_bytes(g[10:13], 'little'),
                               2 * int.from_bytes(g[13:16], 'little')))
            if sample:
                ptr = b2.peek(syms['prof_ptr'], 2)
                end = ptr[0] | ptr[1] << 8
                n = (end - syms['prof_buf']) // 2
                if end >= syms['prof_end']:
                    full += 1
                data = b2.peek(syms['prof_buf'], 2 * n)
                pcs.update(data[2 * j] | data[2 * j + 1] << 8 for j in range(n))
        print('%-18s %-18s %5d frames%s' % (build, name, len(ps), ', %d batches filled the buffer' % full
                                            if full else ''), flush=True)
    return pcs, frames


def listing(build):
    """The listing's instructions: address -> (routine, source text, the
    nearest label, local or not)."""
    insns = {}
    routine = label = None
    depth = 0
    with open(os.path.join(b2.BUILD, build + '.lst')) as f:
        for line in f:
            text = line.rstrip('\n')
            body = re.sub(r'^\s*[0-9A-F]{4}\s+', '', text)
            m = re.match(r'^\s*([0-9A-F]{4})\s+\.(\w+)\s*$', text)
            if m:
                label = m.group(2)
            if m and depth == 0:
                routine = label
            if body.strip() == '{':
                depth += 1
            elif body.strip() == '}':
                depth = max(0, depth - 1)
            m = re.match(r'^\s*([0-9A-F]{4})\s+((?:[0-9A-F]{2} )+)\s*(.*)$', text)
            if m:
                insns[int(m.group(1), 16)] = (routine, m.group(3).strip(), routine + '.' + label
                                              if label != routine else routine)
    return insns


def report(pcs, frames):
    insns = listing('harness_prof')
    addrs = sorted(insns)
    total = sum(pcs.values())

    def owner(pc):
        # the sample is the instruction about to run: the one at pc, or the
        # nearest before it (a macro's expansion lines carry no address)
        lo, hi = 0, len(addrs)
        while lo < hi:
            mid = (lo + hi) // 2
            if addrs[mid] <= pc:
                lo = mid + 1
            else:
                hi = mid
        return addrs[lo - 1] if lo else None

    by_insn = collections.Counter()
    for pc, n in pcs.items():
        a = owner(pc)
        if a is not None:
            by_insn[a] += n
    by_routine = collections.Counter()
    for a, n in by_insn.items():
        by_routine[insns[a][0]] += n

    clear = [c for c, _, _ in frames]
    cap = [c for _, c, _ in frames]
    ren = [r for _, _, r in frames]
    tot = sorted(a + b + c for a, b, c in frames)
    print('\n%d frames: clear %d, capture %d, render (plot, sprites, fill) %d cycles a frame, mean;'
          ' a whole frame median %d, 90%% %d, max %d; %d cycles in all'
          % (len(frames), statistics.mean(clear), statistics.mean(cap), statistics.mean(ren),
             tot[len(tot) // 2], tot[len(tot) * 9 // 10], tot[-1], sum(tot)))
    if not total:
        return
    print('%d samples\n\nby routine:' % total)
    for r, n in by_routine.most_common(40):
        print('  %5.2f%%  %s' % (100 * n / total, r))

    by_label = collections.Counter()
    for a, n in by_insn.items():
        by_label[insns[a][2]] += n
    print('\nby label (from each label to the next):')
    for r, n in by_label.most_common(50):
        print('  %5.2f%%  %s' % (100 * n / total, r))

    # overlapping 64-byte windows, every 16 bytes
    win = collections.Counter()
    for a, n in by_insn.items():
        for start in range((a - 48) & ~15, a + 1, 16):
            if start <= a < start + 64:
                win[start] += n
    print('\nhottest 64-byte windows (overlapping, every 16 bytes):')
    shown = []
    for start, n in win.most_common(200):
        if any(abs(start - s) < 64 for s in shown):
            continue
        shown.append(start)
        first = owner(start) or start
        print('  %5.2f%%  &%04X-&%04X  %s' % (100 * n / total, start, start + 63, insns.get(first, ('?',))[0]))
        if len(shown) == 30:
            break

    print('\nhottest instructions:')
    for a, n in by_insn.most_common(40):
        print('  %5.2f%%  &%04X  %-16s %s' % (100 * n / total, a, insns[a][0], insns[a][1]))

    with open(os.path.join(b2.BUILD, 'profile.txt'), 'w') as f:
        f.write('%d samples over %d frames; each line: share of samples, address, routine, source\n\n'
                % (total, len(frames)))
        for a in addrs:
            n = by_insn.get(a, 0)
            f.write('%6.2f%%  &%04X  %-16s %s\n' % (100 * n / total, a, insns[a][0] or '', insns[a][1]))
    print('\n(the whole listing with its shares: build/profile.txt)')


def main(argv):
    bench = len(argv) > 1 and argv[1] == 'bench'
    pcs, frames = run(sample=not bench)
    report(pcs, frames)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
