"""replay.py - unit replay on the Beeb (PLAN.md 5.6, REPLAY.md): records
decoded, their before-state poked into the Beeb's shapes, the call made
through the mailbox, and the after-state read back, re-encoded the same way
and compared byte for byte."""

import struct

FN_NAMES = ['own_piece', 'chain_add', 'close_columns', 'run_end', 'merge', 'flip', 'hide', 'meet',
            'clip_line', 'merge_above', 'inst_new', 'seg_record', 'inst_issue', 'visit_of', 'enter',
            'process_edge', 'make_line', 'edge_line', 'transform', 'col_gt', 'col_ge']
CX_SIZE = 19
LINE_SIZE = 9
PIECE_SIZE = 9
VISIT_SIZE = 29
INST_SIZE = 17
SLOTS = 4
MAX_CELLS = 32                          # the C's PT_MAX_CELLS: cell_visit's length in a record
MAX_INSTS = 8
CMD_REPLAY = 6

ARGS_SIZE = {0: CX_SIZE + LINE_SIZE + 5, 1: CX_SIZE + 4, 2: 5, 3: CX_SIZE, 4: CX_SIZE + LINE_SIZE + 8,
             5: CX_SIZE + LINE_SIZE + 9, 6: CX_SIZE + LINE_SIZE + 7, 7: CX_SIZE + LINE_SIZE + 8,
             8: CX_SIZE + LINE_SIZE + 2, 9: CX_SIZE + LINE_SIZE + 2, 10: 1, 11: 1, 12: 1, 13: 1, 14: 12,
             15: 1, 16: 18, 17: 39, 18: 4, 19: 9, 20: 9}
RESULTS_SIZE = {0: CX_SIZE + 1, 1: CX_SIZE, 2: 0, 3: CX_SIZE, 4: CX_SIZE, 5: CX_SIZE + 1, 6: CX_SIZE + 1,
                7: CX_SIZE + 1, 8: CX_SIZE, 9: CX_SIZE, 10: 1, 11: 0, 12: 0, 13: 1, 14: 1, 15: 1, 16: LINE_SIZE,
                17: 34 + LINE_SIZE, 18: 8, 19: 1, 20: 1}
NO_FS = {16, 18, 19, 20}                # the pure ones carry no frame state

# The pool's pages, in a record piece's order; the visit's tables, in a
# record visit's order up to its inst (then active[4], from vs_act)
POOL = ['p_x0', 'p_x1', 'p_y0', 'p_yf', 'p_glo', 'p_ghi', 'p_ce', 'p_co', 'p_next']
VISIT = ['vs_cell', 'vs_k', 'vs_x', 'vs_xr', 'vs_tcur', 'vs_bcur', 'vs_tmax', 'vs_bmin'] \
    + ['vs0_' + f for f in ('xl', 'xh', 'yl', 'yh', 'sx', 'fl', 'f', 'e')] \
    + ['vs1_' + f for f in ('xl', 'xh', 'yl', 'yh', 'sx', 'fl', 'f', 'e')] + ['vs_inst']
INST = ['in_sprite', 'in_cx0', 'in_cx1', 'in_seg', 'in_segt', 'in_prev', 'in_next', 'in_scl', 'in_sch',
        'in_l0', 'in_l1', 'in_l2', 'in_t0', 'in_t1', 'in_t2']


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
    """A frame state, as raw bytes and its parts."""
    start = rd.pos
    pool_num = rd.u16()
    pieces = [rd.take(PIECE_SIZE) for _ in range(pool_num)]
    num_visits = rd.u8()
    visits = [rd.take(VISIT_SIZE) for _ in range(num_visits)]
    cell_visit = rd.take(MAX_CELLS)
    num_insts = rd.u8()
    insts = [rd.take(INST_SIZE) for _ in range(num_insts)]
    order = rd.take(2)                      # order_head, order_tail
    seg_num = rd.u16()
    segs = rd.take(seg_num)
    num_pend = rd.u8()
    pend = rd.take(num_pend)
    return {'raw': rd.data[start:rd.pos], 'pool_num': pool_num, 'pieces': pieces, 'visits': visits,
            'cell_visit': cell_visit, 'insts': insts, 'order': order, 'seg_num': seg_num, 'segs': segs,
            'pend': pend}


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
        before = read_fs(rd) if fn not in NO_FS else None
        after = read_fs(rd)['raw'] if fn not in NO_FS else b''
        results = rd.take(RESULTS_SIZE[fn])
        if rd.pos != pos + 2 + n:
            raise ValueError('%s: record length mismatch' % path)
        yield fn, cam, args, before, after, results
        pos += 2 + n


def inst_tables(insts):
    """The instances as the Beeb's tables, MAX_INSTS each: left and top in 24 bits."""
    out = [bytearray(MAX_INSTS) for _ in INST]
    for i, rec in enumerate(insts):
        sprite, cx0, cx1, seg, segt, prev, nxt, scale, left, top = struct.unpack('<7BHii', rec)
        assert -(1 << 23) <= left < (1 << 23) and -(1 << 23) <= top < (1 << 23)
        vals = [sprite, cx0, cx1, seg, segt, prev, nxt, scale & 255, scale >> 8,
                left & 255, (left >> 8) & 255, (left >> 16) & 255, top & 255, (top >> 8) & 255, (top >> 16) & 255]
        for t, v in enumerate(vals):
            out[t][i] = v
    return out


class Pokes:
    """Writes gathered, then sent as few requests as the addresses allow:
    the tables lie in runs, so adjacent writes go together."""

    def __init__(self):
        self.mem = {}

    def __call__(self, addr, data):
        for i, b in enumerate(data):
            self.mem[addr + i] = b

    def send(self, b2):
        addrs = sorted(self.mem)
        i = 0
        while i < len(addrs):
            j = i
            while j + 1 < len(addrs) and addrs[j + 1] == addrs[j] + 1:
                j += 1
            b2.poke(addrs[i], bytes(self.mem[a] for a in addrs[i:j + 1]))
            i = j + 1


class Peeks:
    """Reads gathered, then made as a few blocks (small gaps read too)."""

    def __init__(self, b2, spans, gap=64):
        self.blocks = []
        for a, e in sorted((a, a + n) for a, n in spans):
            if self.blocks and a <= self.blocks[-1][1] + gap:
                self.blocks[-1][1] = max(self.blocks[-1][1], e)
            else:
                self.blocks.append([a, e])
        self.data = [bytes(b2.peek(a, e - a)) for a, e in self.blocks]

    def __call__(self, addr, n):
        for (a, e), d in zip(self.blocks, self.data):
            if a <= addr and addr + n <= e:
                return d[addr - a:addr - a + n]
        raise ValueError('not read: &%04X' % addr)


def poke_fs(b2, syms, fs):
    """A record's frame state into the Beeb's tables, a field a table, and
    its counts into the staging (rp_pool, rp_nvis). Every table goes in
    whole, zero past the record's entries, as the C replays into a fresh
    renderer: a visit made by the call starts from zeros on both."""
    visits = syms['VISITS']
    poke = Pokes()
    for k, name in enumerate(POOL):
        page = bytearray(256)
        for i, p in enumerate(fs['pieces']):
            page[i] = p[k]
        poke(syms[name], bytes(page))
    assert len(fs['visits']) <= visits
    for k, name in enumerate(VISIT):
        poke(syms[name], bytes(v[k] for v in fs['visits']).ljust(visits, b'\x00'))
    act = bytearray(SLOTS * visits)
    for i, v in enumerate(fs['visits']):
        for s in range(SLOTS):
            act[s * visits + i] = v[len(VISIT) + s]
    poke(syms['vs_act'], bytes(act))
    cells = syms['MAP_NUM_CELLS']
    assert not any(fs['cell_visit'][cells:])
    poke(syms['cell_visit'], fs['cell_visit'][:cells])
    for name, t in zip(INST, inst_tables(fs['insts'])):
        poke(syms[name], bytes(t))
    segs = bytearray(256)
    segs[:fs['seg_num']] = fs['segs']
    poke(syms['segs'], bytes(segs))
    poke(syms['pend'], fs['pend'].ljust(32, b'\x00'))
    poke(syms['rp_pool'], bytes([(fs['pool_num'] - 1) & 0xFF]))
    poke(syms['rp_nvis'], bytes([len(fs['visits']), len(fs['insts']), fs['order'][0], fs['order'][1],
                                 fs['seg_num'] & 0xFF, len(fs['pend'])]))
    poke.send(b2)


def peek_fs(b2, syms):
    """The frame state the Beeb leaves, encoded as a record's."""
    visits = syms['VISITS']
    cells = syms['MAP_NUM_CELLS']
    spans = [(syms[name], 256) for name in POOL] + [(syms[name], visits) for name in VISIT] \
        + [(syms['vs_act'], SLOTS * visits), (syms['cell_visit'], cells), (syms['segs'], 256), (syms['pend'], 32),
           (syms['rp_pool'], 1), (syms['rp_nvis'], 6)] + [(syms[name], MAX_INSTS) for name in INST]
    peek = Peeks(b2, spans)
    pool_last = peek(syms['rp_pool'], 1)[0]
    num_visits, num_insts, head, tail, seg_next, num_pend = peek(syms['rp_nvis'], 6)
    pages = [peek(syms[name], 256) for name in POOL]
    out = bytearray(struct.pack('<H', pool_last + 1))
    for i in range(pool_last + 1):
        out += bytes(page[i] for page in pages)
    tables = [peek(syms[name], visits) for name in VISIT]
    act = peek(syms['vs_act'], SLOTS * visits)
    out.append(num_visits)
    for i in range(num_visits):
        out += bytes(t[i] for t in tables) + bytes(act[s * visits + i] for s in range(SLOTS))
    out += peek(syms['cell_visit'], cells).ljust(MAX_CELLS, b'\x00')
    out.append(num_insts)
    insts = [peek(syms[name], MAX_INSTS) for name in INST]
    for i in range(num_insts):
        f = [t[i] for t in insts]
        left = f[9] | f[10] << 8 | f[11] << 16
        top = f[12] | f[13] << 8 | f[14] << 16
        left -= (left & 0x800000) << 1      # sign-extended from 24 bits
        top -= (top & 0x800000) << 1
        out += struct.pack('<7BHii', *f[:7], f[7] | f[8] << 8, left, top)
    out += bytes([head, tail])
    seg_num = seg_next or 256
    out += struct.pack('<H', seg_num) + peek(syms['segs'], 256)[:seg_num]
    out.append(num_pend)
    return bytes(out + peek(syms['pend'], 32)[:num_pend])


def replay(b2, mb, syms, fn, cam, args, before):
    """One record on the Beeb: the after half it gives (FS, results), an
    error, and the call's microseconds."""
    if before is not None:
        poke_fs(b2, syms, before)
    b2.poke(syms['rp_cam'], cam)
    b2.poke(syms['rp_buf'], args)
    err, us = mb.call(CMD_REPLAY, (fn,))
    if err:
        return None, 'error %d' % err, 0
    results = bytes(b2.peek(syms['rp_res'], RESULTS_SIZE[fn]))
    if before is None:
        return results, None, us
    return peek_fs(b2, syms) + results, None, us


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
        got, err, us = replay(b2, mb, syms, fn, cam, args, before)
        if times is not None:
            times.append(us)
        want = after + results
        if err:
            bad.append((k, err))
        elif got != want:
            bad.append((k, describe_diff(want, got)))
    return n, bad
