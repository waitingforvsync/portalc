#!/usr/bin/env python3
"""spike.py - the recursion spike's model (test/spike.6502), step for step.

    spike.py [SEED...]      print the model's results for each seed

spike(seed) returns what the Beeb leaves at spike_out: the checksum, the
portals walked, the children the depth cap closed and the deepest level.
b2.py's spike command runs the Beeb's and compares.
"""

import sys

MAX_DEPTH = 16                          # PT_MAX_DEPTH
HOLD = [12, 10, 9, 6, 3, 4]             # bytes each chain link holds across its call


def addc(r, v, n):
    """r plus n copies of v, byte by byte with the carry, from CLC."""
    c = 0
    for _ in range(n):
        s = r + v + c
        r, c = s & 255, s >> 8
    return r


def spike(seed):
    st = {'sum': 0, 'nodes': 0, 'closes': 0, 'npend': 0, 'dhigh': 0, 'depth': 1}

    def mix(b):
        s = st['sum']
        s = ((s << 1) | (s >> 15)) & 0xFFFF
        st['sum'] = s ^ (b & 255)

    def link(k, v, p):
        # link k: v' = 5v + 1 + byte, held across the next link; the leaf
        # (k = 6, mulg) takes bmin EOR depth
        b = p[k] if k < 6 else p[6] ^ st['depth']
        v1 = (v * 5 + 1 + b) & 255
        if k == 6:
            return v1
        return addc(link(k + 1, v1, p), v1, HOLD[k])

    def process_edge(p, steps):
        v0 = p[0] ^ steps
        r = addc(link(1, v0, p), v0, HOLD[0])
        if steps == 3:
            has = True
        elif steps == 2:
            has = (r & 7) == 0
        else:
            has = (r & 7) == 1
        return has, [(p[k] + r + k) & 255 for k in range(7)]

    def walk(p):
        pend = st['npend']
        st['dhigh'] = max(st['dhigh'], st['depth'])
        while True:
            st['npend'] = (st['npend'] + 1) & 255
            st['nodes'] += 1
            for b in p:
                mix(b)
            steps = 4
            tail = False
            while steps:
                steps -= 1
                has, child = process_edge(p, steps)
                if steps == 1:              # the last edge
                    if has:
                        p = child
                        tail = True
                    break
                if has:
                    if st['depth'] >= MAX_DEPTH:
                        st['closes'] += 1
                        mix(child[0])
                    else:
                        st['depth'] += 1
                        walk(child)
                        st['depth'] -= 1
                mix(p[0])
                mix(steps)
            if not tail:
                break
        mix(pend)

    walk([seed, 0, 128, 1, 2, 0, 64])
    return {'sum': st['sum'], 'nodes': st['nodes'] & 0xFFFF, 'closes': st['closes'] & 0xFFFF,
            'dhigh': st['dhigh']}


if __name__ == '__main__':
    for a in sys.argv[1:] or ['0']:
        print(a, spike(int(a, 0)))
