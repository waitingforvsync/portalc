"""replay.py - unit replay on the Beeb (PLAN.md 5.6, REPLAY.md): records
decoded, their before-state poked into the Beeb's shapes, the call made
through the mailbox, and the after-state read back, re-encoded the same way
and compared byte for byte."""

import os
import struct

FN_NAMES = ['emit', 'resolve_block', 'clip_line', 'clip_edge_line', 'cut', 'inst_new', 'seg_record',
            'inst_issue', 'child_portal', 'next_open', 'close_columns', 'process_edge']
CX_SIZE = 35
LINE_SIZE = 7
VISIT_SIZE = 24
INST_SIZE = 17
SLOTS = 4
VISITS = 24
CMD_REPLAY = 6


class Reader:
    def __init__(self, data, pos=0):
        self.data = data
        self.pos = pos

    def take(self, n):
        b = self.data[self.pos:self.pos + n]
        if len(b) != n:
            raise ValueError('record ends early')
        self.pos += n
        return b

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack('<H', self.take(2))[0]


def read_fs(rd):
    """A frame state, as raw bytes and the parts replay needs."""
    start = rd.pos
    pool_num = rd.u16()
    pieces = [rd.take(8) for _ in range(pool_num)]
    num_visits = rd.u8()
    visits = [rd.take(VISIT_SIZE) for _ in range(num_visits)]
    cell_visit = rd.take(VISITS)
    num_insts = rd.u8()
    insts = [rd.take(INST_SIZE) for _ in range(num_insts)]
    order = rd.take(2)                      # order_head, order_tail
    seg_num = rd.u16()
    segs = rd.take(seg_num)
    num_pend = rd.u8()
    pend = rd.take(num_pend)
    return {'raw': rd.data[start:rd.pos], 'pool_num': pool_num, 'pieces': pieces, 'visits': visits,
            'num_visits': num_visits, 'pieces_end': 2 + 8 * pool_num, 'cell_visit': cell_visit,
            'insts': insts, 'order': order, 'seg_num': seg_num, 'segs': segs, 'pend': pend}


ARGS_SIZE = {0: CX_SIZE + LINE_SIZE + 3, 1: CX_SIZE + LINE_SIZE + 2, 2: CX_SIZE + LINE_SIZE + 1,
             3: CX_SIZE + LINE_SIZE + 4, 4: 3, 5: 1, 6: 7, 7: 1, 8: 15, 9: 19, 10: 5, 11: 7, 12: 18,
             13: 39, 14: 4, 15: 9, 16: 9}
RESULTS_SIZE = {0: CX_SIZE, 1: CX_SIZE, 2: CX_SIZE, 3: CX_SIZE, 4: 2, 5: 1, 6: 0, 7: 0, 8: 8, 9: 4, 10: 0,
                11: 15, 12: 7, 13: 41, 14: 8, 15: 1, 16: 1}
HAS_FS = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 13}    # the pure ones carry no frame state
STAGED = {5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}  # arguments in rp_buf, results in rp_res
WALK = {5, 6, 7, 8, 10, 11}             # the visits, cell_visit and sprites go in and come back too
VTABLES = 20                            # a visit's tables (scratch.6502): cell k xr, v0, v1, inst
POOL = ['p_x0', 'p_x1', 'p_y0', 'p_glo', 'p_ghi', 'p_ce', 'p_co', 'p_next']
INST = ['in_sprite', 'in_cx0', 'in_cx1', 'in_seg', 'in_segt', 'in_prev', 'in_next', 'in_scl', 'in_sch',
        'in_l0', 'in_l1', 'in_l2', 'in_t0', 'in_t1', 'in_t2']
MAX_INSTS = 8


def records(path):
    """Yield (fn, CAM, before-args, before-FS, after-FS raw, results raw) per record."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'PCRP':
        raise ValueError('%s: not a replay file' % path)
    fn = data[5]
    pos = 8
    while pos < len(data):
        n = struct.unpack('<H', data[pos:pos + 2])[0]
        rd = Reader(data[:pos + 2 + n], pos + 2)
        cam = rd.take(9)
        args = rd.take(ARGS_SIZE[fn])
        before = read_fs(rd) if fn in HAS_FS else None
        after = read_fs(rd)['raw'] if fn in HAS_FS else b''
        results = rd.take(RESULTS_SIZE[fn])
        if rd.pos != pos + 2 + n:
            raise ValueError('%s: record length mismatch' % path)
        yield fn, cam, args, before, after, results
        pos += 2 + n


def pool_pages(pieces):
    """The pool as the Beeb's eight pages: x0 x1 y0 glo ghi ce co next."""
    pages = [bytearray(256) for _ in range(8)]
    for i, p in enumerate(pieces):
        for k in range(8):
            pages[k][i] = p[k]
    return pages


def poke_pool(b2, syms, pieces):
    """The pool into its pages (D13: they're not all together)."""
    for name, page in zip(POOL, pool_pages(pieces)):
        b2.poke(syms[name], bytes(page))


def peek_pool(b2, syms):
    """The pool back, as one 2048-byte block in POOL's order."""
    return b''.join(bytes(b2.peek(syms[name], 256)) for name in POOL)


def vis_act(visits):
    """The visits' active pieces as the Beeb keeps them: slot * 24 + visit."""
    out = bytearray(SLOTS * VISITS)
    for v, rec in enumerate(visits):
        for s in range(SLOTS):
            out[s * VISITS + v] = rec[20 + s]
    return bytes(out)


def visit_tables(visits):
    """The visits as the Beeb keeps them (spare.6502): a table a field, 24 each."""
    out = bytearray(VTABLES * VISITS)
    for v, rec in enumerate(visits):
        for t in range(VTABLES):
            out[t * VISITS + v] = rec[t]
    return bytes(out)


def inst_tables(insts):
    """The instances as the Beeb's tables (bss.6502), MAX_INSTS each: left and
    top in 24 bits."""
    out = [bytearray(MAX_INSTS) for _ in INST]
    for i, rec in enumerate(insts):
        sprite, cx0, cx1, seg, segt, prev, nxt, scale, left, top = struct.unpack('<7BHii', rec)
        assert -(1 << 23) <= left < (1 << 23) and -(1 << 23) <= top < (1 << 23)
        vals = [sprite, cx0, cx1, seg, segt, prev, nxt, scale & 255, scale >> 8,
                left & 255, (left >> 8) & 255, (left >> 16) & 255, top & 255, (top >> 8) & 255, (top >> 16) & 255]
        for t, v in enumerate(vals):
            out[t][i] = v
    return out


def encode_walk_fs(pool, pool_last, act, tables, cell_visit, num_visits, insts, counts, segs, pend):
    """The after FS from the Beeb's pool, visits, cell_visit and sprite state.
    counts: num_insts, order_head, order_tail, seg_next (seg_num, 0 for 256),
    num_pend."""
    num_insts, head, tail, seg_next, num_pend = counts
    num = pool_last + 1
    out = bytearray(struct.pack('<H', num))
    for i in range(num):
        out += bytes(pool[k * 256 + i] for k in range(8))
    out.append(num_visits)
    for v in range(num_visits):
        out += bytes(tables[t * VISITS + v] for t in range(VTABLES))
        out += bytes(act[s * VISITS + v] for s in range(SLOTS))
    out += bytes(cell_visit)
    out.append(num_insts)
    for i in range(num_insts):
        f = [t[i] for t in insts]
        left = f[9] | f[10] << 8 | f[11] << 16
        top = f[12] | f[13] << 8 | f[14] << 16
        left -= (left & 0x800000) << 1      # sign-extended from 24 bits
        top -= (top & 0x800000) << 1
        out += struct.pack('<7BHii', *f[:7], f[7] | f[8] << 8, left, top)
    out += bytes([head, tail])
    seg_num = seg_next or 256
    out += struct.pack('<H', seg_num) + bytes(segs[:seg_num])
    out.append(num_pend)
    return bytes(out + bytes(pend[:num_pend]))


def encode_fs(before, pool, pool_last, act):
    """The after FS from the Beeb's pool and active pieces, the rest as before
    (nothing the clip group calls touches it)."""
    num = pool_last + 1
    out = bytearray(struct.pack('<H', num))
    for i in range(num):
        out += bytes(pool[k * 256 + i] for k in range(8))
    raw = before['raw']
    rest = bytearray(raw[before['pieces_end']:])
    for v in range(before['num_visits']):
        for s in range(SLOTS):
            rest[1 + v * VISIT_SIZE + 20 + s] = act[s * VISITS + v]
    return bytes(out + rest)


def replay_staged(b2, mb, syms, fn, cam, args, before, after=None):
    """The targets staged through rp_buf and rp_res."""
    if before is not None:
        poke_pool(b2, syms, before['pieces'])
        b2.poke(syms['vis_act'], vis_act(before['visits']))
        b2.poke(syms['rp_pool'], bytes([(before['pool_num'] - 1) & 0xFF]))
        if fn in WALK:
            vt = visit_tables(before['visits'])
            b2.poke(syms['vs_cell'], vt[:10 * VISITS])
            b2.poke(syms['vs0_e'], vt[10 * VISITS:])
            b2.poke(syms['cell_visit'], before['cell_visit'])
            for name, t in zip(INST, inst_tables(before['insts'])):
                b2.poke(syms[name], bytes(t))
            segs = bytearray(256)
            segs[:before['seg_num']] = before['segs']
            b2.poke(syms['segs'], bytes(segs))
            b2.poke(syms['pend'], before['pend'] or b'\x00')
            b2.poke(syms['rp_nvis'], bytes([before['num_visits'], len(before['insts']),
                                             before['order'][0], before['order'][1],
                                             before['seg_num'] & 0xFF, len(before['pend'])]))
    b2.poke(syms['rp_cam'], cam)
    b2.poke(syms['rp_buf'], args)
    err, us = mb.call(CMD_REPLAY, (fn,))
    if err:
        return None, 'error %d' % err, 0
    results = bytes(b2.peek(syms['rp_res'], RESULTS_SIZE[fn]))
    if before is None:
        return results, None, us
    pool = peek_pool(b2, syms)
    act = b2.peek(syms['vis_act'], SLOTS * VISITS)
    last = b2.peek(syms['rp_pool'], 1)[0]
    if fn in WALK:
        tables = bytes(b2.peek(syms['vs_cell'], 10 * VISITS)) + bytes(b2.peek(syms['vs0_e'], 10 * VISITS))
        cv = b2.peek(syms['cell_visit'], VISITS)
        counts = b2.peek(syms['rp_nvis'], 6)
        insts = [b2.peek(syms[name], MAX_INSTS) for name in INST]
        segs = b2.peek(syms['segs'], 256)
        pend = b2.peek(syms['pend'], 32)
        return encode_walk_fs(pool, last, act, tables, cv, counts[0], insts, counts[1:], segs,
                              pend) + results, None, us
    return encode_fs(before, pool, last, act) + results, None, us


def replay_one(b2, mb, syms, fn, args, before):
    """Run one record on the Beeb; the after half it gives (FS, results)."""
    poke_pool(b2, syms, before['pieces'])
    b2.poke(syms['vis_act'], vis_act(before['visits']))
    stage = bytearray(1 + CX_SIZE + LINE_SIZE + 4 + 2)
    stage[0] = (before['pool_num'] - 1) & 0xFF
    if fn == 4:
        stage[1 + CX_SIZE + LINE_SIZE:1 + CX_SIZE + LINE_SIZE + 3] = args
    else:
        stage[1:1 + CX_SIZE + LINE_SIZE] = args[:CX_SIZE + LINE_SIZE]
        extra = args[CX_SIZE + LINE_SIZE:]
        stage[1 + CX_SIZE + LINE_SIZE:1 + CX_SIZE + LINE_SIZE + len(extra)] = extra
    b2.poke(syms['rp_pool'], bytes(stage))
    err, us = mb.call(CMD_REPLAY, (fn,))
    if err:
        return None, 'error %d' % err, 0
    pool = peek_pool(b2, syms)
    act = b2.peek(syms['vis_act'], SLOTS * VISITS)
    got = b2.peek(syms['rp_pool'], len(stage))
    fs = encode_fs(before, pool, got[0], act)
    if fn == 4:
        results = bytes(got[1 + CX_SIZE + LINE_SIZE + 4:1 + CX_SIZE + LINE_SIZE + 6])
    else:
        results = bytes(got[1:1 + CX_SIZE])
    return fs + results, None, us


def describe_diff(want, got):
    for i, (a, b) in enumerate(zip(want, got)):
        if a != b:
            return 'first difference at byte %d of the after half: C &%02X, Beeb &%02X' % (i, a, b)
    return 'lengths differ: C %d, Beeb %d' % (len(want), len(got))


def baseline(mb):
    """The timer's own microseconds, around nothing."""
    return min(mb.call(CMD_REPLAY, (255,))[1] for _ in range(8))


def check(b2, mb, syms, path, limit=None, times=None, which=None):
    """Replay a file's records (those of map which, if given: the Beeb holds
    one map a build): (count, failures as (index, why)); each call's
    microseconds appended to times."""
    bad = []
    n = 0
    for k, (fn, cam, args, before, after, results) in enumerate(records(path)):
        if limit is not None and k >= limit:
            break
        if which is not None and cam[0] != which:
            continue
        n += 1
        if fn in STAGED:
            got, err, us = replay_staged(b2, mb, syms, fn, cam, args, before, after)
        else:
            got, err, us = replay_one(b2, mb, syms, fn, args, before)
        if times is not None:
            times.append(us)
        want = after + results
        if err:
            bad.append((k, err))
        elif got != want:
            bad.append((k, describe_diff(want, got)))
    return n, bad
