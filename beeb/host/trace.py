#!/usr/bin/env python3
"""trace.py - render.c and arith.c as the replay recorder and the cost model
see them (PLAN.md D1c, 5.6), generated at build time: no engine edits.

    trace.py SRC_DIR OUT_DIR

Writes OUT_DIR/render_traced.c, OUT_DIR/arith_traced.c and
OUT_DIR/trace_counters.h:

- Every function in WRAP is renamed NAME__impl, and a wrapper takes its
  name: tr_before_NAME(args), the call, tr_after_NAME(args[, ret]). The
  hooks are beebtool's (host/trace_hooks.c), defined after the file.
- Every counter in COUNTERS is a TC(name) or TCN(name, k) statement,
  inserted at a function's entry or at an anchor line inside it.

Each anchor must match exactly as often as it says, and each named function
must exist exactly once, so a change to the engine that moves the ground
under a probe fails the build rather than quietly miscounting. #line
directives keep compiler messages pointing at the real sources.
"""

import os
import re
import sys

# the unit-replay targets (PLAN.md 5.6)
WRAP = ['emit', 'resolve_block', 'clip_line', 'clip_edge_line', 'cut', 'inst_new', 'seg_record',
        'inst_issue', 'child_portal', 'next_open', 'close_columns', 'process_edge']

# The cost model's counters (host/trace_hooks.c weights them), in report
# order. Each probe is (file, function, anchor, times, where, amount):
#   anchor None     at the function's entry
#   anchor regex    at every line of the function matching it; there must be
#                   exactly `times` of them
#   where           'before' or 'after' the anchor line
#   amount          None for TC (one), else a C expression for TCN
COUNTERS = [
    # capture
    ('transform',        [('render.c', 'transform', None, 1, None, None)]),
    ('rot',              [('arith.c', 'pt_rot', None, 1, None, None)]),
    ('recip16',          [('arith.c', 'pt_recip16', None, 1, None, None)]),
    ('recip8',           [('arith.c', 'pt_recip8', None, 1, None, None)]),
    ('project_col',      [('arith.c', 'pt_project_col', None, 1, None, None)]),
    ('project_rowd',     [('arith.c', 'pt_project_rowd', None, 1, None, None)]),
    ('project_q8',       [('arith.c', 'pt_project_q8', None, 1, None, None)]),
    ('grad_mag',         [('arith.c', 'pt_grad_mag', None, 1, None, None)]),
    ('gradq8_mag',       [('arith.c', 'pt_gradq8_mag', None, 1, None, None)]),
    ('div_t',            [('arith.c', 'pt_div_t', None, 1, None, None)]),
    ('div_cols',         [('arith.c', 'pt_div_cols', None, 1, None, None)]),
    ('mulg',             [('arith.c', 'pt_mulg_u16', None, 1, None, None)]),
    ('piece_y',          [('render.c', 'piece_y', None, 1, None, None)]),
    ('line_y',           [('render.c', 'line_y', None, 1, None, None)]),
    ('process_edge',     [('render.c', 'process_edge', None, 1, None, None)]),
    ('edge_work',        [('render.c', 'process_edge', r'^\s*p->x = xb;$', 1, 'after', None)]),
    ('make_line',        [('render.c', 'make_line', None, 1, None, None)]),
    ('near_clip',        [('render.c', 'make_line', r'^\s*// exactly one endpoint too close', 1, 'after', None)]),
    ('line_reuse',       [('render.c', 'edge_line', r'^\s*const pt_piece \*pp = &el->r->pool\[q\];$', 1, 'after', None)]),
    ('clip_open',        [('render.c', 'clip_line', r'^\s*if \(st == ST_OPEN\) \{$', 1, 'after', None)]),
    ('clip_forced',      [('render.c', 'clip_line', r'^\s*uint8_t c = L->x0;$', 1, 'after',
                           '(st != ST_OPEN)')]),
    ('clip_fast',        [('render.c', 'clip_line', r'^\s*emit\(cx, L, L->x0, L->x1, ST_VISIBLE\);$', 1,
                           'before', None)]),
    ('resolve_block',    [('render.c', 'resolve_block', None, 1, None, None)]),
    ('crossing',         [('render.c', 'crossing', None, 1, None, None)]),
    ('emit_extend',      [('render.c', 'emit', r'^\s*r->pool\[q\]\.x1 = r1;$', 1, 'before', None)]),
    ('emit_visible',     [('render.c', 'emit', r'^\s*q = pool_alloc\(r, r0, r1,', 1, 'before', None)]),
    ('emit_merge',       [('render.c', 'emit', r'^\s*uint8_t \*pp = \(st == ST_ABOVE\)', 1, 'before', None)]),
    ('pool_split',       [('render.c', 'pool_split', None, 1, None, None)]),
    ('cut',              [('render.c', 'cut', None, 1, None, None)]),
    ('child_portal',     [('render.c', 'child_portal', None, 1, None, None)]),
    ('walk_entry',       [('render.c', 'walk', r'^\s*seg_record\(r, &p\);$', 1, 'before', None)]),
    # plot
    ('plot_pieces',      [('render.c', 'plot', r'^\s*if \(pt_g_mag\(p->g\) < 0x100\)$', 1, 'before', None)]),
    ('plot_flat_pieces', [('render.c', 'plot_shallow', r'^\s*if \(gm == 0\) \{$', 1, 'after', None)]),
    ('plot_flat_bytes',  [('render.c', 'plot_shallow', r'^\s*if \(gm == 0\) \{$', 1, 'after',
                           '(((uint8_t)(p->x1 - 1) >> 2) - (p->x0 >> 2)) + 1')]),
    ('plot_sh_bytes',    [('render.c', 'byte_down', None, 1, None, None),
                          ('render.c', 'byte_up', None, 1, None, None)]),
    ('plot_sh_cap_px',   [('render.c', 'plot_shallow', r'^\s*pt_mode5_eor\(fb, cur, mask, pat\);$', 4,
                           'before', None)]),
    ('plot_steep_cols',  [('render.c', 'plot_steep', None, 1, None, 'p->x1 - p->x0')]),
    # sprites
    ('spr_bytecols',     [('render.c', 'sprite_pass', r'^\s*r->num_togs = 0;$', 1, 'before', None)]),
    ('spr_rows_scanned', [('render.c', 'sprite_pass', r'^\s*r->num_togs = 0;$', 1, 'before', 'ymax + 1')]),
    ('spr_pixcols',      [('render.c', 'sprite_pass', r'^\s*uint8_t j = inst_col\(in, c\);$', 1, 'before',
                           None)]),
    ('spr_paints',       [('render.c', 'paint', None, 1, None, None)]),
    ('spr_puts',         [('render.c', 'tog_put', r'^\s*tog_plot\(r, bc, row, e, o\);$', 1, 'before', None)]),
    ('spr_erases',       [('render.c', 'tog_erase', r'^\s*tog_plot\(r, bc, t->row, e, o\);', 1, 'before',
                           None)]),
]


class TraceError(Exception):
    pass


def match_paren(src, i):
    """The index just past the ')' matching the '(' at src[i]."""
    depth = 0
    for j in range(i, len(src)):
        if src[j] == '(':
            depth += 1
        elif src[j] == ')':
            depth -= 1
            if depth == 0:
                return j + 1
    raise TraceError('unbalanced parentheses')


def find_def(src, name):
    """The one definition of name (header at column 0, body '{' at column 0,
    closed by '}' at column 0): (header start, '(' index, ')' end, body
    start, body end just past the closing brace's newline)."""
    found = []
    for m in re.finditer(r'^[a-z][^\n;{}()]*?\b%s\(' % re.escape(name), src, re.M):
        paren = m.end() - 1
        close = match_paren(src, paren)
        if not src.startswith('\n{\n', close):
            continue    # a prototype or a call
        body = close + 1
        end = src.index('\n}\n', body) + 3
        found.append((m.start(), paren, close, body, end))
    if len(found) != 1:
        raise TraceError('%s: %d definitions' % (name, len(found)))
    return found[0]


def split_params(text):
    """Split a parameter list at its top-level commas."""
    out = []
    depth = 0
    cur = ''
    for ch in text:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def line_of(src, i):
    return src.count('\n', 0, i) + 1


def probes_for(fname):
    """{function: [(counter, anchor, times, where, amount)]} for one file."""
    out = {}
    for counter, probes in COUNTERS:
        for f, func, anchor, times, where, amount in probes:
            if f == fname:
                out.setdefault(func, []).append((counter, anchor, times, where, amount))
    return out


def stmt(counter, amount):
    return 'TC(%s);' % counter if amount is None else 'TCN(%s, %s);' % (counter, amount)


def transform(src, path, fname, wrap):
    """The file with its probes inserted and its wrap targets renamed; plus
    the wrappers' text, to append."""
    # Edits as (position, text, kind), applied from the end backwards so the
    # positions stay valid. A #line after each insertion puts the following
    # line back at its real number.
    edits = []
    for func, probes in probes_for(fname).items():
        start, paren, close, body, end = find_def(src, func)
        for counter, anchor, times, where, amount in probes:
            if anchor is None:
                at = src.index('\n', body) + 1    # the line after the body's '{'
                edits.append((at, '    %s\n#line %d "%s"\n' % (stmt(counter, amount), line_of(src, at), path)))
                continue
            hits = [m for m in re.finditer(anchor, src[body:end], re.M)]
            if len(hits) != times:
                raise TraceError('%s: anchor %r in %s matched %d times, not %d'
                                 % (counter, anchor, func, len(hits), times))
            for m in hits:
                # the anchor's line is the one holding the match's last
                # character (a leading \s* can start on a blank line above)
                last = body + m.end() - 1
                ls = src.rfind('\n', 0, last) + 1
                le = src.index('\n', last) + 1
                at = ls if where == 'before' else le
                indent = re.match(r'[ \t]*', src[at:]).group(0)    # the line the probe precedes
                edits.append((at, '%s%s\n#line %d "%s"\n' % (indent, stmt(counter, amount), line_of(src, at), path)))

    wrappers = []
    protos = []
    for func in wrap:
        start, paren, close, body, end = find_def(src, func)
        head = src[start:paren]                      # 'static void name'
        ret = head[:head.rindex(func)].strip()       # 'static void'
        params = src[paren + 1:close - 1]
        names = [re.search(r'(\w+)\s*$', p).group(1) for p in split_params(params)]
        edits.append((paren, '__impl', 'rename'))
        edits.append((end, '%s %s(%s);\n#line %d "%s"\n' % (ret, func, params, line_of(src, end), path)))
        rtype = ret.replace('static', '').strip()
        args = ', '.join(names)
        protos.append('static void tr_before_%s(%s);' % (func, params))
        if rtype == 'void':
            protos.append('static void tr_after_%s(%s);' % (func, params))
            wrappers.append('%s %s(%s)\n{\n    tr_before_%s(%s);\n    %s__impl(%s);\n    tr_after_%s(%s);\n}\n'
                            % (ret, func, params, func, args, func, args, func, args))
        else:
            protos.append('static void tr_after_%s(%s, %s ret);' % (func, params, rtype))
            wrappers.append('%s %s(%s)\n{\n    tr_before_%s(%s);\n    %s ret = %s__impl(%s);\n'
                            '    tr_after_%s(%s, ret);\n    return ret;\n}\n'
                            % (ret, func, params, func, args, rtype, func, args, func, args))

    for at, text, *kind in sorted(edits, key=lambda e: e[0], reverse=True):
        src = src[:at] + text + src[at:]
    return src, protos, wrappers


def main(src_dir, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    names = [c for c, _ in COUNTERS]
    with open(os.path.join(out_dir, 'trace_counters.h'), 'w') as f:
        f.write('// trace_counters.h - generated by beeb/host/trace.py: do not edit.\n')
        f.write('#ifndef TRACE_COUNTERS_H_\n#define TRACE_COUNTERS_H_\n\n')
        f.write('enum {\n%s    TC_N,\n};\n\n' % ''.join('    TC_%s,\n' % n for n in names))
        f.write('static unsigned long tc_[TC_N];\n')
        f.write('static const char *const tc_names_[TC_N] = {\n%s};\n\n' % ''.join('    "%s",\n' % n for n in names))
        f.write('#define TC(n) (tc_[TC_##n]++)\n#define TCN(n, k) (tc_[TC_##n] += (unsigned long)(k))\n\n')
        f.write('#endif\n')

    for fname, wrap in (('arith.c', []), ('render.c', WRAP)):
        path = os.path.abspath(os.path.join(src_dir, fname))
        with open(path) as f:
            src = f.read()
        out, protos, wrappers = transform(src, path, fname, wrap)
        base = fname.replace('.c', '_traced.c')
        with open(os.path.join(out_dir, base), 'w') as f:
            f.write('// %s - generated by beeb/host/trace.py from %s: do not edit.\n' % (base, fname))
            f.write('#include "trace_counters.h"\n#line 1 "%s"\n' % path)
            f.write(out)
            if wrappers:
                f.write('\n#line %d "%s"\n' % (out.count('\n') + 4, os.path.abspath(os.path.join(out_dir, base))))
                f.write('// the recording hooks (host/trace_hooks.c, after this file)\n')
                f.write('\n'.join(protos) + '\n\n')
                f.write('// the wrappers: each target under its own name\n')
                f.write('\n'.join(wrappers))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1], sys.argv[2]))
    except TraceError as e:
        print('trace.py: %s' % e, file=sys.stderr)
        sys.exit(1)
