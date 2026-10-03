#!/usr/bin/env python3
"""frame.py - one frame of the release, cycle by cycle, on emu6502.py.

    frame.py X Y CELL ANGLE [MAP]   where one frame's cycles go: the stages;
                                    capture by job, and by routine inclusive
                                    and exclusive; the portal tree, with each
                                    subtree's cycles; the transforms, visits,
                                    edges and lines; the pool's pieces and
                                    what made them; the row evaluations and
                                    what asked for them
    frame.py sweep X Y CELL [MAP]   all 256 angles: capture and render cycles,
                                    pieces and visits
    frame.py check X Y CELL [MAP]   all 256 angles' images against the C's

No b2: it loads the release's own image (build/portal/PORTAL, or
build/portal_mapN/ for MAP N, so build that first), runs start up to the
main loop, then calls clear, capture, plot, sprite_pass and fill. Nothing
interrupts, so b2's timings run about half a percent higher: its vsyncs,
and the timer calls round each stage.
"""

import bisect
import collections
import os
import re
import struct
import subprocess
import sys

from emu6502 import CPU

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, '..', 'build')
TOOL = os.path.join(HERE, '..', '..', 'build', 'beeb', 'host', 'portalc_beebtool')
SENT = 0xFFF0                       # the stages return here: nothing else does
STAGES = ('clear', 'capture', 'plot', 'sprite_pass', 'fill')
ST_OPEN = 3                         # clip.6502's statuses


def build_name(mp):
    return 'portal' if mp == 0 else 'portal_map%d' % mp


def symbols(build):
    import json
    with open(os.path.join(BUILD, build + '.json')) as f:
        d = json.load(f)
    out = {}
    for syms in d.values():
        out.update((k, v) for k, v in syms.items() if isinstance(v, int))
    return out


def listing(build):
    """address -> (routine, routine.label), as profile.py reads it."""
    insns = {}
    routine = label = None
    depth = 0
    with open(os.path.join(BUILD, build + '.lst')) as f:
        for line in f:
            text = line.rstrip('\n')
            body = re.sub(r'^\s*[0-9A-F]{4}\s+', '', text).strip()
            m = re.match(r'^\s*([0-9A-F]{4})\s+\.(\w+)\s*$', text)
            if m:
                label = m.group(2)
                if depth == 0:
                    routine = label
            if body == '{':
                depth += 1
            elif body == '}':
                depth = max(0, depth - 1)
            m = re.match(r'^\s*([0-9A-F]{4})\s+(?:[0-9A-F]{2} )+', text)
            if m:
                insns[int(m.group(1), 16)] = (routine, routine + '.' + label if label != routine else routine)
    return insns


class Machine:
    def __init__(self, mp=0):
        self.build = build_name(mp)
        self.s = symbols(self.build)
        self.m = bytearray(65536)
        with open(os.path.join(BUILD, self.build, 'PORTAL'), 'rb') as f:
            img = f.read()
        n = self.s['entry'] - self.s['payload']
        self.m[0x300:0x300 + n] = img[:n]
        self.cpu = CPU(self.m)
        self.cpu.pc = self.s['start']
        while self.cpu.pc != self.s['mainloop']:
            self.cpu.step()
        self.base = bytes(self.m)

    def pose(self, x, y, cell, angle):
        """Back to just after start, at this pose."""
        self.m[:] = self.base
        self.cpu.sp = 0xF0
        for name, v, n in (('fr_camx', x, 2), ('fr_camy', y, 2), ('fr_angle', angle, 1), ('fr_cell', cell, 1)):
            for k in range(n):
                self.m[self.s[name] + k] = v >> (8 * k) & 0xFF

    def call(self, name, before=None, after=None):
        """Call a routine through to its return: its cycles."""
        cpu = self.cpu
        cpu.push((SENT - 1) >> 8)
        cpu.push((SENT - 1) & 0xFF)
        cpu.pc = self.s[name]
        c0 = cpu.cycles
        if before is None:
            while cpu.pc != SENT:
                cpu.step()
        else:
            while cpu.pc != SENT:
                pc, c = cpu.pc, cpu.cycles
                before(pc)
                cpu.step()
                after(pc, cpu.cycles - c)
        return cpu.cycles - c0

    def frame(self, x, y, cell, angle):
        """The stages' cycles (capture's carry set: dropped, and no more)."""
        self.pose(x, y, cell, angle)
        out = {}
        for st in STAGES:
            out[st] = self.call(st)
            if st == 'capture' and self.cpu.c:
                out['dropped'] = True
                break
        return out

    def image(self):
        b = self.m[self.s['scrnbase']] << 8
        return bytes(self.m[b:b + 4096])


# capture's routines, by job
JOBS = (
    ('clipping against the bounds', {'clip_line', 'clip_edge_line', 'resolve_block', 'emit', 'cut', 'pool_split',
                                     'chain_extents', 'chain_append', 'crossing', 'grad_diff', 'div_cols', 'piece_at',
                                     'closed_add', 'next_open', 'close_columns'}),
    ('row evaluations (row_step, piece_y, line_y)', {'row_step', 'piece_y', 'line_y'}),
    ('making lines (side clips, projections, gradients)',
     {'make_line', 'edge_line', 'project_rowd', 'row_from', 'project_col', 'project_col_q8', 'grad_mag', 'gradq8_mag',
      'div_t', 'div_q8', 'lerp16', 'clip_right', 'clip_left', 'ep_unclipped', 'ep_recip', 'ep_depth', 'set_h',
      'col_gt', 'col_ge', 'col_behind'}),
    ('vertex transforms', {'rot', 'transform', 'tf_vertex', 'tf_edge', 'vs_set1', 'vs_shift', 'vs_next1'}),
    ('the walk (process_edge, visits, children, roots)', {'process_edge', 'walk', 'child_portal', 'visit_new',
                                                          'visit_advance', 'root_portal', 'root_exact', 'muls16',
                                                          'capture'}),
    ('sprites (inst_new, seg_record, inst_issue)', {'inst_new', 'seg_record', 'seg_pieces', 'inst_issue', 'project_q8',
                                                    'inst_art'}),
)


class Trace:
    """Everything one frame does, from hooks round each instruction."""

    def __init__(self, mc, x, y, cell, angle):
        self.mc, self.x, self.y = mc, x, y
        s, m = mc.s, mc.m
        self.s, self.m = s, m
        insns = listing(mc.build)
        addrs = sorted(insns)
        self.owner = {}
        for a in range(0x300, 0x5100):
            i = bisect.bisect_right(addrs, a) - 1
            self.owner[a] = insns[addrs[i]] if i >= 0 else ('?', '?')
        self.names = {}
        for k, v in sorted(s.items(), key=lambda t: '.' in t[0]):
            if 0x300 <= v < 0x5100 and '@' not in k:
                self.names.setdefault(v, k)
        self.excl = collections.Counter()           # (stage, routine) -> cycles
        self.incl = collections.Counter()           # capture's routines, inclusive
        self.calls = collections.Counter()
        self.active = collections.Counter()
        self.stack = []                             # [name, cycles at the JSR, sp after it, tag]
        self.levels = []                            # (sp after JSR walk, the node then)
        self.nodes, self.cur = [], None
        self.tf, self.visits, self.edges = [], [], []
        self.lines = collections.Counter()          # make_line by (cell, edge, height)
        self.origin = {1: ('root',), 2: ('root',)}
        self.evals, self.evcyc = collections.Counter(), collections.Counter()
        self.fast = collections.Counter()
        self.emits = collections.Counter()
        self.hooks = {s['walk.portal']: self.portal, s['tf_vertex']: lambda: self.tf.append(mc.cpu.x),
                      s['visit_new']: lambda: self.visits.append(mc.cpu.x), s['process_edge']: self.edge,
                      s['make_line']: self.line, s['piece_y']: self.row, s['line_y']: self.row,
                      s['pool_split']: self.split, s['emit.fresh']: self.fresh,
                      s['emit.grow']: lambda: self.emits.update(['grown']), s['clip_line']: self.clip,
                      s['clip_line.place']: self.missed}
        mc.pose(x, y, cell, angle)
        self.tot = {}
        for st in STAGES:
            self.stage = st
            self.tot[st] = mc.call(st, self.before, self.after)

    # ---- per instruction ----
    def before(self, pc):
        cpu, m = self.mc.cpu, self.m
        self.node = self.cur
        op = m[pc]
        self.ret = None
        if op == 0x20:
            t = m[pc + 1] | m[pc + 2] << 8
            nm = self.names.get(t, '%04X' % t)
            if self.stage == 'capture':
                self.calls[nm] += 1
            self.stack.append([nm, cpu.cycles, (cpu.sp - 2) & 0xFF, None])
            self.active[nm] += 1
            if t == self.s['walk']:
                self.levels.append(((cpu.sp - 2) & 0xFF, self.cur))
        elif op == 0x60 and self.stack and self.stack[-1][2] == cpu.sp:
            self.ret = self.stack.pop()
        h = self.hooks.get(pc)
        if h and self.stage == 'capture':
            h()

    def after(self, pc, cyc):
        self.excl[(self.stage, self.owner.get(pc, ('?',))[0])] += cyc
        if self.node is not None:
            self.node['self'] += cyc
        if self.ret:
            nm, c0, sp, tag = self.ret
            self.active[nm] -= 1
            d = self.mc.cpu.cycles - c0
            if self.stage == 'capture' and self.active[nm] == 0:
                self.incl[nm] += d
            if tag:
                self.evcyc[tag] += d
            if self.levels and self.levels[-1][0] == sp:
                self.cur = self.levels.pop()[1]

    # ---- hooks ----
    def b(self, name, i=0):
        return self.m[self.s[name] + i]

    def caller(self):
        sp = self.mc.cpu.sp
        a = (self.m[0x100 + ((sp + 1) & 0xFF)] | self.m[0x100 + ((sp + 2) & 0xFF)] << 8) - 2
        return self.owner.get(a, ('?', '?'))[1]

    def vert(self, i):
        return self.b('vx_lo', i) | self.b('vx_hi', i) << 8, self.b('vy_lo', i) | self.b('vy_hi', i) << 8

    def edge_verts(self, cell, k):
        f, n = self.b('c_first', cell), self.b('c_num', cell)
        return self.b('e_v0', f + k), self.b('e_v0', f + (k + 1) % n)

    def dist(self, a, b):
        (ax, ay), (bx, by) = self.vert(a), self.vert(b)
        dx, dy = bx - ax, by - ay
        t = 0 if dx == dy == 0 else max(0, min(1, ((self.x - ax) * dx + (self.y - ay) * dy) / (dx * dx + dy * dy)))
        return ((self.x - ax - t * dx) ** 2 + (self.y - ay - t * dy) ** 2) ** 0.5

    def portal(self):
        n = dict(cell=self.b('po_cell'), x=self.b('po_x'), xr=self.b('po_xr'), self=0, kids=[], dist=0)
        if self.cur is not None:
            cell = self.cur['cell']
            v = self.b('cell_visit', cell)
            n['dist'] = self.dist(*self.edge_verts(cell, self.b('vs_k', v)))
            self.cur['kids'].append(n)
        self.nodes.append(n)
        self.cur = self.node = n

    def edge(self):
        cell = self.b('po_cell')
        self.edges.append((cell, self.b('vs_k', self.b('cell_visit', cell))))

    def line(self):
        cell = self.b('po_cell')
        self.lines[(cell, self.b('vs_k', self.b('cell_visit', cell)), self.b('h_m'), self.b('h_below'))] += 1

    def row(self):
        cpu = self.mc.cpu
        if cpu.pc == self.s['piece_y']:
            who, x0, x1 = 'piece', self.b('p_x0', cpu.x), self.b('p_x1', cpu.x)
        else:
            who, x0, x1 = 'line', self.b('l_x0'), self.b('l_x1')
        where = 'at x0' if cpu.a == x0 else 'at x1-1' if cpu.a == x1 - 1 else 'inside'
        tag = (who, where, self.caller())
        self.evals[tag] += 1
        self.stack[-1][3] = tag

    def split(self):
        how = self.caller()
        if how.startswith('cut'):
            how += ', a soft edge' if self.b('e_colour', self.b('process_edge.ei')) == 0 else ', a drawn edge'
        elif not (self.b('l_ce') | self.b('l_co')):
            how += ', a colour-0 line'
        self.origin[self.b('pool_last') + 1] = ('split by ' + how,)

    def fresh(self):
        act = self.b('vis_act', self.b('cx_act'))
        if act == 0:
            why = 'no active piece'
        elif self.b('p_x1', act) != self.b('emit.r0'):
            why = 'active piece ends elsewhere'
        elif (self.b('p_glo', act), self.b('p_ghi', act)) != (self.b('l_g'), self.b('l_g', 1)):
            why = 'gradient differs'
        elif (self.b('p_ce', act), self.b('p_co', act)) != (self.b('l_ce'), self.b('l_co')):
            why = 'colour differs'
        else:
            why = 'another piece is the chain tail'
        self.origin[self.b('pool_last') + 1] = ('new line piece: ' + why,)
        self.emits['fresh, ' + why] += 1

    def clip(self):
        self.fast['open' if self.b('clip_line.st') == ST_OPEN else 'known side'] += 1

    def missed(self):
        if self.b('clip_line.st') == ST_OPEN:
            self.fast['open, fast path missed'] += 1

    # ---- the report ----
    def pieces(self):
        P = {}
        for i in range(1, self.b('pool_last') + 1):
            P[i] = dict(x0=self.b('p_x0', i), x1=self.b('p_x1', i), y0=self.b('p_y0', i),
                        g=self.b('p_glo', i) | self.b('p_ghi', i) << 8, ce=self.b('p_ce', i), co=self.b('p_co', i))
        return P

    def report(self):
        tot, out = self.tot, []
        p = out.append
        whole = sum(tot.values())
        cap = tot['capture']
        p('%s, (%d, %d) in cell %d, angle %d: %d cycles, %.2f vsyncs' % (
            self.mc.build, self.x, self.y, self.m[self.s['fr_cell']], self.m[self.s['fr_angle']], whole, whole / 40000))
        for st in STAGES:
            p('  %-12s %7d  %5.1f%%' % (st, tot[st], 100 * tot[st] / whole))

        ex = collections.Counter({r: v for (st, r), v in self.excl.items() if st == 'capture'})
        p('\nCapture by job (exclusive):')
        seen = set()
        for job, rs in JOBS:
            v = sum(ex[r] for r in rs)
            seen |= rs
            p('  %-52s %7d  %5.1f%%' % (job, v, 100 * v / cap))
        rest = sum(v for r, v in ex.items() if r not in seen)
        p('  %-52s %7d  %5.1f%%' % ('other', rest, 100 * rest / cap))

        p('\nCapture by routine: inclusive (calls, a call) | exclusive')
        inc = sorted(self.incl.items(), key=lambda t: -t[1])[:24]
        exc = ex.most_common(24)
        for i in range(max(len(inc), len(exc))):
            a = '%-26s %7d %4d %6.0f' % (inc[i][0], inc[i][1], self.calls[inc[i][0]],
                                        inc[i][1] / max(1, self.calls[inc[i][0]])) if i < len(inc) else ' ' * 46
            b = '%-16s %7d' % exc[i] if i < len(exc) else ''
            p('  %s | %s' % (a, b))

        p('\nThe render, exclusive:')
        for st in STAGES[2:] + STAGES[:1]:
            top = sorted(((r, v) for (s2, r), v in self.excl.items() if s2 == st), key=lambda t: -t[1])[:4]
            p('  %-12s %s' % (st, ', '.join('%s %d' % t for t in top)))

        dup = collections.Counter(self.tf)
        p('\nVertices: %d transforms, %d distinct; twice or more: %s' % (
            len(self.tf), len(dup), ', '.join('%d (%d)' % t for t in sorted(dup.items()) if t[1] > 1) or 'none'))
        p('Cells visited: %d %s' % (len(self.visits), self.visits))
        p('Portals walked: %d' % len(self.nodes))
        ec = collections.Counter(self.edges)
        p('Edges processed: %d, %d distinct' % (len(self.edges), len(ec)))
        p('Lines projected: %d, %d distinct (cell, edge, height)' % (sum(self.lines.values()), len(self.lines)))
        p('Line clips: %s' % dict(self.fast))
        p('Visible emits: %s' % dict(self.emits))

        p('\nPortal tree: cell, columns, the entry edge\'s distance; own and subtree cycles')

        def sub(n):
            return n['self'] + sum(sub(k) for k in n['kids'])

        def show(n, ind):
            p('  %s%2d [%3d,%3d) %5.0f: %6d %6d' % ('  ' * ind, n['cell'], n['x'], n['xr'], n['dist'], n['self'], sub(n)))
            for k in n['kids']:
                show(k, ind + 1)
        if self.nodes:
            show(self.nodes[0], 0)

        P = self.pieces()
        plotted = [i for i, q in P.items() if (q['ce'] | q['co']) and not (q['y0'] >= 64 and not q['g'] & 0x8000)]
        p('\nPieces: %d; %d plotted, %d colour 0, %d wholly below' % (
            len(P), len(plotted), sum(1 for q in P.values() if not (q['ce'] | q['co'])),
            len(P) - len(plotted) - sum(1 for q in P.values() if not (q['ce'] | q['co']))))
        p('  made by:')
        for k, n in collections.Counter(o[0] for o in self.origin.values()).most_common():
            p('    %3d %s' % (n, k))

        def row_at(q, c):
            gm = q['g'] & 0x7FFF
            y = q['y0'] * 256 + 128 + (-gm if q['g'] & 0x8000 else gm) * (c - q['x0'])
            return min(64, max(0, y >> 8))

        same = collections.Counter()
        for i, a in P.items():
            for j, c in P.items():
                if (c['x0'] == a['x1'] and c['g'] == a['g'] and (c['ce'], c['co']) == (a['ce'], a['co'])
                        and row_at(a, c['x0']) == c['y0']):
                    same[(self.origin.get(j, ('?',))[0], 'plotted' if j in plotted else 'colour 0')] += 1
        p('  same line and colour as the piece just left of it, made by:')
        for (how, pl), n in same.most_common():
            p('    %3d %s (%s)' % (n, how, pl))

        p('\nRow evaluations, by whose row, where, and who asked: count, cycles')
        for k in sorted(self.evals, key=lambda t: -self.evcyc[t]):
            p('  %-6s %-8s %-28s %4d %6d' % (k[0], k[1], k[2], self.evals[k], self.evcyc[k]))
        p('  %d in all, %d cycles' % (sum(self.evals.values()), sum(self.evcyc.values())))
        return '\n'.join(out)


def angles(x, y, cell):
    return [(x, y, a, cell) for a in range(256)]


def sweep(mp, x, y, cell):
    mc = Machine(mp)
    print('angle capture  render pieces visits')
    for a in range(256):
        r = mc.frame(x, y, cell, a)
        print('%5d %7d %7d %6d %6d' % (a, r['capture'], sum(v for k, v in r.items() if k not in ('capture', 'dropped')),
                                      mc.m[mc.s['pool_last']], mc.m[mc.s['num_visits']] - 1), flush=True)


def check(mp, x, y, cell):
    import tempfile
    poses = angles(x, y, cell)
    with tempfile.TemporaryDirectory() as d:
        pin, pout = os.path.join(d, 'poses.bin'), os.path.join(d, 'frames.bin')
        with open(pin, 'wb') as f:
            f.write(b''.join(struct.pack('<HHBB', *p) for p in poses))
        subprocess.run([TOOL, 'render', str(mp), pin, pout], check=True)
        with open(pout, 'rb') as f:
            want = f.read()
    mc = Machine(mp)
    bad = 0
    for k, (px, py, a, c) in enumerate(poses):
        mc.frame(px, py, c, a)
        if mc.image() != want[k * 4096:(k + 1) * 4096]:
            bad += 1
            print('angle %d differs' % a)
    print('%d angles, %d differ from the C' % (len(poses), bad))
    return bad == 0


def main(argv):
    if argv and argv[0] in ('sweep', 'check'):
        args = list(map(int, argv[1:]))
        mp = args[3] if len(args) > 3 else 0
        if argv[0] == 'sweep':
            sweep(mp, *args[:3])
        else:
            sys.exit(0 if check(mp, *args[:3]) else 1)
    elif len(argv) >= 4:
        args = list(map(int, argv))
        mp = args[4] if len(args) > 4 else 0
        print(Trace(Machine(mp), *args[:4]).report())
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == '__main__':
    main(sys.argv[1:])
