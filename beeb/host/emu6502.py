"""emu6502.py - a 6502 that counts its cycles, for frame.py.

NMOS, the documented opcodes only, and no decimal mode (SED stops it, as
does BRK or anything undocumented). The cycles are the data sheet's: the
base count, one more for a read through abs,X, abs,Y or (zp),Y that
crosses a page, one more for a branch taken and another if it lands on
another page. There's no hardware: &FE00 and friends are plain memory, and
nothing interrupts.
"""

IMM, ZP, ZPX, ZPY, ABS, ABSX, ABSY, INDX, INDY, IMP, ACC, REL, IND = range(13)


class CPU:
    def __init__(self, mem):
        self.m = mem
        self.a = self.x = self.y = 0
        self.sp = 0xFF
        self.pc = 0
        self.n = self.v = self.z = self.c = 0
        self.i = 1
        self.d = 0
        self.cycles = 0
        self.build()

    # ---- addressing ----
    def ea(self, mode):
        m = self.m
        pc = self.pc
        if mode == ZP:
            self.pc = pc + 1
            return m[pc], 0
        if mode == ZPX:
            self.pc = pc + 1
            return (m[pc] + self.x) & 0xFF, 0
        if mode == ZPY:
            self.pc = pc + 1
            return (m[pc] + self.y) & 0xFF, 0
        if mode == ABS:
            self.pc = pc + 2
            return m[pc] | (m[pc + 1] << 8), 0
        if mode == ABSX:
            self.pc = pc + 2
            b = m[pc] | (m[pc + 1] << 8)
            a = (b + self.x) & 0xFFFF
            return a, (a ^ b) >> 8 != 0
        if mode == ABSY:
            self.pc = pc + 2
            b = m[pc] | (m[pc + 1] << 8)
            a = (b + self.y) & 0xFFFF
            return a, (a ^ b) >> 8 != 0
        if mode == INDX:
            self.pc = pc + 1
            z = (m[pc] + self.x) & 0xFF
            return m[z] | (m[(z + 1) & 0xFF] << 8), 0
        if mode == INDY:
            self.pc = pc + 1
            z = m[pc]
            b = m[z] | (m[(z + 1) & 0xFF] << 8)
            a = (b + self.y) & 0xFFFF
            return a, (a ^ b) >> 8 != 0
        raise ValueError(mode)

    def setnz(self, v):
        self.n = v >> 7
        self.z = v == 0
        return v

    def getp(self):
        return (self.n << 7) | (self.v << 6) | 0x30 | (self.d << 3) | (self.i << 2) | (self.z << 1) | self.c

    def setp(self, p):
        self.n = p >> 7 & 1
        self.v = p >> 6 & 1
        self.d = p >> 3 & 1
        self.i = p >> 2 & 1
        self.z = p >> 1 & 1
        self.c = p & 1

    def push(self, v):
        self.m[0x100 + self.sp] = v
        self.sp = (self.sp - 1) & 0xFF

    def pull(self):
        self.sp = (self.sp + 1) & 0xFF
        return self.m[0x100 + self.sp]

    # ---- the opcode table ----
    def build(self):
        t = {}
        cpu = self

        def rd(op, mode, base, fn, pagex=True):
            def f():
                if mode == IMM:
                    v = cpu.m[cpu.pc]
                    cpu.pc += 1
                    cpu.cycles += base
                else:
                    a, cross = cpu.ea(mode)
                    v = cpu.m[a]
                    cpu.cycles += base + (1 if cross and pagex else 0)
                fn(v)
            t[op] = f

        def wr(op, mode, base, get):
            def f():
                a, _ = cpu.ea(mode)
                cpu.m[a] = get()
                cpu.cycles += base
            t[op] = f

        def rmw(op, mode, base, fn):
            if mode == ACC:
                def f():
                    cpu.a = fn(cpu.a)
                    cpu.cycles += 2
            else:
                def f():
                    a, _ = cpu.ea(mode)
                    cpu.m[a] = fn(cpu.m[a])
                    cpu.cycles += base
            t[op] = f

        def imp(op, fn, base=2):
            def f():
                fn()
                cpu.cycles += base
            t[op] = f

        def lda(v): cpu.a = cpu.setnz(v)
        def ldx(v): cpu.x = cpu.setnz(v)
        def ldy(v): cpu.y = cpu.setnz(v)
        def ora(v): cpu.a = cpu.setnz(cpu.a | v)
        def and_(v): cpu.a = cpu.setnz(cpu.a & v)
        def eor(v): cpu.a = cpu.setnz(cpu.a ^ v)

        def adc(v):
            if cpu.d:
                raise RuntimeError('decimal mode at %04X' % cpu.pc)
            s = cpu.a + v + cpu.c
            cpu.v = (~(cpu.a ^ v) & (cpu.a ^ s) & 0x80) != 0
            cpu.c = s >> 8
            cpu.a = cpu.setnz(s & 0xFF)

        def sbc(v):
            adc(v ^ 0xFF)

        def cmp_(r):
            def f(v):
                s = r() - v
                cpu.c = s >= 0
                cpu.setnz(s & 0xFF)
            return f

        def bit(v):
            cpu.z = (cpu.a & v) == 0
            cpu.n = v >> 7
            cpu.v = v >> 6 & 1

        def asl(v):
            cpu.c = v >> 7
            return cpu.setnz((v << 1) & 0xFF)

        def lsr(v):
            cpu.c = v & 1
            return cpu.setnz(v >> 1)

        def rol(v):
            r = ((v << 1) | cpu.c) & 0xFF
            cpu.c = v >> 7
            return cpu.setnz(r)

        def ror(v):
            r = (v >> 1) | (cpu.c << 7)
            cpu.c = v & 1
            return cpu.setnz(r)

        def inc(v): return cpu.setnz((v + 1) & 0xFF)
        def dec(v): return cpu.setnz((v - 1) & 0xFF)

        groups = [
            (lda, {0xA9: (IMM, 2), 0xA5: (ZP, 3), 0xB5: (ZPX, 4), 0xAD: (ABS, 4), 0xBD: (ABSX, 4),
                   0xB9: (ABSY, 4), 0xA1: (INDX, 6), 0xB1: (INDY, 5)}),
            (ldx, {0xA2: (IMM, 2), 0xA6: (ZP, 3), 0xB6: (ZPY, 4), 0xAE: (ABS, 4), 0xBE: (ABSY, 4)}),
            (ldy, {0xA0: (IMM, 2), 0xA4: (ZP, 3), 0xB4: (ZPX, 4), 0xAC: (ABS, 4), 0xBC: (ABSX, 4)}),
            (ora, {0x09: (IMM, 2), 0x05: (ZP, 3), 0x15: (ZPX, 4), 0x0D: (ABS, 4), 0x1D: (ABSX, 4),
                   0x19: (ABSY, 4), 0x01: (INDX, 6), 0x11: (INDY, 5)}),
            (and_, {0x29: (IMM, 2), 0x25: (ZP, 3), 0x35: (ZPX, 4), 0x2D: (ABS, 4), 0x3D: (ABSX, 4),
                    0x39: (ABSY, 4), 0x21: (INDX, 6), 0x31: (INDY, 5)}),
            (eor, {0x49: (IMM, 2), 0x45: (ZP, 3), 0x55: (ZPX, 4), 0x4D: (ABS, 4), 0x5D: (ABSX, 4),
                   0x59: (ABSY, 4), 0x41: (INDX, 6), 0x51: (INDY, 5)}),
            (adc, {0x69: (IMM, 2), 0x65: (ZP, 3), 0x75: (ZPX, 4), 0x6D: (ABS, 4), 0x7D: (ABSX, 4),
                   0x79: (ABSY, 4), 0x61: (INDX, 6), 0x71: (INDY, 5)}),
            (sbc, {0xE9: (IMM, 2), 0xE5: (ZP, 3), 0xF5: (ZPX, 4), 0xED: (ABS, 4), 0xFD: (ABSX, 4),
                   0xF9: (ABSY, 4), 0xE1: (INDX, 6), 0xF1: (INDY, 5)}),
            (cmp_(lambda: cpu.a), {0xC9: (IMM, 2), 0xC5: (ZP, 3), 0xD5: (ZPX, 4), 0xCD: (ABS, 4),
                                   0xDD: (ABSX, 4), 0xD9: (ABSY, 4), 0xC1: (INDX, 6), 0xD1: (INDY, 5)}),
            (cmp_(lambda: cpu.x), {0xE0: (IMM, 2), 0xE4: (ZP, 3), 0xEC: (ABS, 4)}),
            (cmp_(lambda: cpu.y), {0xC0: (IMM, 2), 0xC4: (ZP, 3), 0xCC: (ABS, 4)}),
            (bit, {0x24: (ZP, 3), 0x2C: (ABS, 4)}),
        ]
        for fn, ops in groups:
            for op, (mode, base) in ops.items():
                rd(op, mode, base, fn)

        for op, (mode, base) in {0x85: (ZP, 3), 0x95: (ZPX, 4), 0x8D: (ABS, 4), 0x9D: (ABSX, 5),
                                 0x99: (ABSY, 5), 0x81: (INDX, 6), 0x91: (INDY, 6)}.items():
            wr(op, mode, base, lambda: cpu.a)
        for op, (mode, base) in {0x86: (ZP, 3), 0x96: (ZPY, 4), 0x8E: (ABS, 4)}.items():
            wr(op, mode, base, lambda: cpu.x)
        for op, (mode, base) in {0x84: (ZP, 3), 0x94: (ZPX, 4), 0x8C: (ABS, 4)}.items():
            wr(op, mode, base, lambda: cpu.y)

        for fn, ops in ((asl, {0x0A: ACC, 0x06: ZP, 0x16: ZPX, 0x0E: ABS, 0x1E: ABSX}),
                        (lsr, {0x4A: ACC, 0x46: ZP, 0x56: ZPX, 0x4E: ABS, 0x5E: ABSX}),
                        (rol, {0x2A: ACC, 0x26: ZP, 0x36: ZPX, 0x2E: ABS, 0x3E: ABSX}),
                        (ror, {0x6A: ACC, 0x66: ZP, 0x76: ZPX, 0x6E: ABS, 0x7E: ABSX}),
                        (inc, {0xE6: ZP, 0xF6: ZPX, 0xEE: ABS, 0xFE: ABSX}),
                        (dec, {0xC6: ZP, 0xD6: ZPX, 0xCE: ABS, 0xDE: ABSX})):
            for op, mode in ops.items():
                rmw(op, mode, {ACC: 2, ZP: 5, ZPX: 6, ABS: 6, ABSX: 7}[mode], fn)

        def setter(name, val):
            def f(): setattr(cpu, name, val)
            return f
        imp(0x18, setter('c', 0)); imp(0x38, setter('c', 1))
        imp(0x58, setter('i', 0)); imp(0x78, setter('i', 1))
        imp(0xB8, setter('v', 0)); imp(0xD8, setter('d', 0))

        def sed():
            raise RuntimeError('SED at %04X' % cpu.pc)
        imp(0xF8, sed)
        imp(0xEA, lambda: None)
        def tax(): cpu.x = cpu.setnz(cpu.a)
        def tay(): cpu.y = cpu.setnz(cpu.a)
        def txa(): cpu.a = cpu.setnz(cpu.x)
        def tya(): cpu.a = cpu.setnz(cpu.y)
        def tsx(): cpu.x = cpu.setnz(cpu.sp)
        def txs(): cpu.sp = cpu.x
        def inx(): cpu.x = cpu.setnz((cpu.x + 1) & 0xFF)
        def iny(): cpu.y = cpu.setnz((cpu.y + 1) & 0xFF)
        def dex(): cpu.x = cpu.setnz((cpu.x - 1) & 0xFF)
        def dey(): cpu.y = cpu.setnz((cpu.y - 1) & 0xFF)
        for op, fn in ((0xAA, tax), (0xA8, tay), (0x8A, txa), (0x98, tya), (0xBA, tsx), (0x9A, txs),
                       (0xE8, inx), (0xC8, iny), (0xCA, dex), (0x88, dey)):
            imp(op, fn)
        imp(0x48, lambda: cpu.push(cpu.a), 3)
        imp(0x08, lambda: cpu.push(cpu.getp()), 3)
        def pla(): cpu.a = cpu.setnz(cpu.pull())
        imp(0x68, pla, 4)
        imp(0x28, lambda: cpu.setp(cpu.pull()), 4)

        def branch(op, test):
            def f():
                off = cpu.m[cpu.pc]
                cpu.pc += 1
                cpu.cycles += 2
                if test():
                    t2 = (cpu.pc + (off - 256 if off & 0x80 else off)) & 0xFFFF
                    cpu.cycles += 1 + ((t2 ^ cpu.pc) >> 8 != 0)
                    cpu.pc = t2
            t[op] = f
        branch(0x10, lambda: not cpu.n); branch(0x30, lambda: cpu.n)
        branch(0x50, lambda: not cpu.v); branch(0x70, lambda: cpu.v)
        branch(0x90, lambda: not cpu.c); branch(0xB0, lambda: cpu.c)
        branch(0xD0, lambda: not cpu.z); branch(0xF0, lambda: cpu.z)

        def jmp():
            cpu.pc = cpu.m[cpu.pc] | (cpu.m[cpu.pc + 1] << 8)
            cpu.cycles += 3
        t[0x4C] = jmp

        def jmpi():
            p = cpu.m[cpu.pc] | (cpu.m[cpu.pc + 1] << 8)
            hi = (p & 0xFF00) | ((p + 1) & 0xFF)       # the NMOS page wrap
            cpu.pc = cpu.m[p] | (cpu.m[hi] << 8)
            cpu.cycles += 5
        t[0x6C] = jmpi

        def jsr():
            target = cpu.m[cpu.pc] | (cpu.m[cpu.pc + 1] << 8)
            ret = cpu.pc + 1
            cpu.push(ret >> 8)
            cpu.push(ret & 0xFF)
            cpu.pc = target
            cpu.cycles += 6
        t[0x20] = jsr

        def rts():
            lo = cpu.pull()
            hi = cpu.pull()
            cpu.pc = ((hi << 8) | lo) + 1
            cpu.cycles += 6
        t[0x60] = rts

        def rti():
            cpu.setp(cpu.pull())
            lo = cpu.pull()
            hi = cpu.pull()
            cpu.pc = (hi << 8) | lo
            cpu.cycles += 6
        t[0x40] = rti

        def brk():
            raise RuntimeError('BRK at %04X' % (cpu.pc - 1))
        t[0x00] = brk

        self.ops = [None] * 256
        for op, f in t.items():
            self.ops[op] = f

    def step(self):
        op = self.m[self.pc]
        f = self.ops[op]
        if f is None:
            raise RuntimeError('illegal opcode %02X at %04X' % (op, self.pc))
        self.pc += 1
        f()
