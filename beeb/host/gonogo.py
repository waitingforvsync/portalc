#!/usr/bin/env python3
"""gonogo.py - the frame-rate go/no-go (PLAN.md 5.9): predicted frame times
from measured costs where Phases 2-4 have them.

    gonogo.py COUNTERS.csv

COUNTERS.csv is `portalc_beebtool_trace cost build/golden COUNTERS.csv`:
a row per golden frame of the traced C's counters, in the cost model's
stream order (sweep, map7, rim, stress, search0..9). Plot is each pose's
plot as S4 timed it on the Beeb (build/s4/*_harness0.cycles, from b2.py
phase4); capture takes S2's measured arithmetic and the cost model's
guesses for the rest; sprites stay guesses until Phase 7.
"""

import csv
import json
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(os.path.dirname(HERE), 'build')

# Cycles a call, S2 (full runs, JOURNAL.md Phase 2). The counters count
# nested calls too, so a routine that calls a counted reciprocal is
# weighted beyond it, as the cost model's own guesses were.
RECIP16 = 92
RECIP8 = 13
MEASURED = {
    'rot': 559,
    'recip16': RECIP16,
    'recip8': RECIP8,
    'project_col': 291,                     # (S2's depths run deeper than the maps')
    'project_rowd': 116,
    'project_q8': 172,
    'grad_mag': 166 - RECIP8,
    'gradq8_mag': 225 - RECIP16,
    'div_t': 252 - RECIP16,                 # (S2's denominators are mostly wide)
    'div_cols': 306 - RECIP16,
    'mulg': 54,
}

# The cost model's guesses for what has no 6502 yet (host/trace_hooks.c).
GUESSES = {
    'transform': 90, 'piece_y': 25, 'line_y': 25, 'process_edge': 60, 'edge_work': 250,
    'make_line': 120, 'near_clip': 250, 'line_reuse': 40, 'clip_open': 90, 'clip_forced': 120,
    'clip_fast': 0, 'resolve_block': 300, 'crossing': 40, 'emit_extend': 60, 'emit_visible': 130,
    'emit_merge': 140, 'pool_split': 90, 'cut': 70, 'child_portal': 160, 'walk_entry': 120,
    'spr_bytecols': 150, 'spr_rows_scanned': 11, 'spr_pixcols': 150, 'spr_paints': 120,
    'spr_puts': 45, 'spr_erases': 45,
}
CALL = 15                                   # a capture call's overhead (PLAN.md 5.9)

CLEAR, FILL = 21868, 37016                  # S3, a buffer each
CAMERA = 5000                               # PLAN.md 4.2's estimate: N steps and one tick
VSYNC = 39936                               # cycles in a 19,968 us frame

STREAMS = ['sweep', 'map7', 'rim', 'stress', 'search0']


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    with open(argv[1]) as f:
        rows = list(csv.DictReader(f))
    plot = []
    for name in STREAMS:
        path = os.path.join(BUILD, 's4', '%s_harness0.cycles' % name)
        with open(path) as f:
            plot += json.load(f)
    if len(plot) != len(rows):
        raise SystemExit('%d frames of counters but %d plots' % (len(rows), len(plot)))

    frames = {}
    for row, p in zip(rows, plot):
        cap = 0.0
        spr = 0.0
        for name, n in row.items():
            if name == 'map' or name.startswith('plot_'):
                continue
            n = int(n)
            if name.startswith('spr_'):
                spr += GUESSES[name] * n
            elif name in MEASURED:
                cap += (MEASURED[name] + CALL) * n
            else:
                cap += (GUESSES[name] + (CALL if GUESSES[name] else 0)) * n
        frames.setdefault(int(row['map']), []).append((cap, p, spr))

    print('Predicted 6502 cycles a frame: capture (S2 arithmetic, the rest guessed, +%d a call),'
          % CALL)
    print('plot (S4, measured), sprites (guessed), and fixed: clear %d + fill %d + camera %d.\n'
          % (CLEAR, FILL, CAMERA))
    print('map frames | capture   plot sprites | median    90%     max | fps: median  90%')
    for m in sorted(frames):
        f = frames[m]
        tot = sorted(c + p + s + CLEAR + FILL + CAMERA for c, p, s in f)
        med, p90, mx = tot[len(tot) // 2], tot[len(tot) * 9 // 10], tot[-1]
        print('%3d %6d | %7.0f %6.0f %7.0f | %6.0f %6.0f %7.0f |      %5.1f %5.1f'
              % (m, len(f), statistics.mean(c for c, _, _ in f), statistics.mean(p for _, p, _ in f),
                 statistics.mean(s for _, _, s in f), med, p90, mx, 2e6 / med, 2e6 / p90))
    print('\n(fps unquantised: a frame is shown at the vsync after it is done, %d cycles apart)' % VSYNC)

    # Shown at vsyncs, over each map's golden frames in order, for two
    # main-loop orders: everything, then post the swap and wait for it; or
    # camera and capture (which never touch a buffer) while the last swap
    # is still pending, then wait, then render and post.
    print('\nshown at vsyncs, frames a second:')
    print('map | update, capture, render, post, wait | update, capture, wait, render, post')
    for m in sorted(frames):
        rates = []
        for overlap in (False, True):
            t = 0.0
            shown = 0.0
            for cap, p, spr in frames[m]:
                render = CLEAR + p + spr + FILL
                if overlap:
                    t = max(t + CAMERA + cap, shown) + render
                else:
                    t = max(t, shown) + CAMERA + cap + render
                shown = -(-t // VSYNC) * VSYNC        # the swap at the next vsync
            rates.append(len(frames[m]) / (shown / VSYNC / 50.08))
        print('%3d | %36.1f | %36.1f' % (m, rates[0], rates[1]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
