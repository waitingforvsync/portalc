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
    rd.take(VISITS)                         # cell_visit
    num_insts = rd.u8()
    rd.take(INST_SIZE * num_insts)
    rd.take(2)                              # order_head, order_tail
    seg_num = rd.u16()
    rd.take(seg_num)
    num_pend = rd.u8()
    rd.take(num_pend)
    return {'raw': rd.data[start:rd.pos], 'pool_num': pool_num, 'pieces': pieces, 'visits': visits,
            'num_visits': num_visits, 'pieces_end': 2 + 8 * pool_num}


ARGS_SIZE = {0: CX_SIZE + LINE_SIZE + 3, 1: CX_SIZE + LINE_SIZE + 2, 2: CX_SIZE + LINE_SIZE + 1,
             3: CX_SIZE + LINE_SIZE + 4, 4: 3, 12: 18, 13: 39, 14: 4, 15: 9, 16: 9}
RESULTS_SIZE = {0: CX_SIZE, 1: CX_SIZE, 2: CX_SIZE, 3: CX_SIZE, 4: 2, 12: 7, 13: 41, 14: 8, 15: 1, 16: 1}
HAS_FS = {0, 1, 2, 3, 4, 13}            # the pure ones carry no frame state
STAGED = {12, 13, 14, 15, 16}           # arguments in rp_buf, results in rp_res


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
    return b''.join(pages)


def vis_act(visits):
    """The visits' active pieces as the Beeb keeps them: slot * 24 + visit."""
    out = bytearray(SLOTS * VISITS)
    for v, rec in enumerate(visits):
        for s in range(SLOTS):
            out[s * VISITS + v] = rec[20 + s]
    return bytes(out)


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


def replay_staged(b2, mb, syms, fn, cam, args, before):
    """The targets staged through rp_buf and rp_res."""
    if before is not None:
        b2.poke(syms['p_x0'], pool_pages(before['pieces']))
        b2.poke(syms['vis_act'], vis_act(before['visits']))
        b2.poke(syms['rp_pool'], bytes([(before['pool_num'] - 1) & 0xFF]))
    b2.poke(syms['rp_cam'], cam)
    b2.poke(syms['rp_buf'], args)
    err, us = mb.call(CMD_REPLAY, (fn,))
    if err:
        return None, 'error %d' % err, 0
    results = bytes(b2.peek(syms['rp_res'], RESULTS_SIZE[fn]))
    if before is None:
        return results, None, us
    pool = b2.peek(syms['p_x0'], 2048)
    act = b2.peek(syms['vis_act'], SLOTS * VISITS)
    last = b2.peek(syms['rp_pool'], 1)[0]
    return encode_fs(before, pool, last, act) + results, None, us


def replay_one(b2, mb, syms, fn, args, before):
    """Run one record on the Beeb; the after half it gives (FS, results)."""
    b2.poke(syms['p_x0'], pool_pages(before['pieces']))
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
    pool = b2.peek(syms['p_x0'], 2048)
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


def check(b2, mb, syms, path, limit=None, times=None):
    """Replay a file's records: (count, failures as (index, why)); each
    call's microseconds appended to times."""
    bad = []
    n = 0
    for k, (fn, cam, args, before, after, results) in enumerate(records(path)):
        if limit is not None and k >= limit:
            break
        n += 1
        if fn in STAGED:
            got, err, us = replay_staged(b2, mb, syms, fn, cam, args, before)
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
