// arith.c - table builders and the arithmetic helpers. The tables are built
// as the 6502 builds them at start up: the quarter squares by recurrence,
// and every table of a smooth function from a bit a delta (below). The
// helpers stick to lookups, adds, shifts and unsigned compares, with widths
// held to what the 6502 routine would keep.

#include "arith.h"

#include <math.h>
#include <stdbool.h>

#include "mode5.h"
#include "richc/macros.h"

// Quarter-square tables: sq1[i] = i*i/4, sq2[i] = (i-255)^2/4, both floored.
// a*b = sq1[a+b] - sq2[a-b+255], exact because a+b and a-b share parity;
// subtracting the other way round gives -(a*b) directly.
static uint16_t sq1_[512];
static uint16_t sq2_[512];

// recipf_[n - 128] = round(0x10000 / n) - 256 for n in [128, 255]: the
// fractional byte of the reciprocal of a normalised denominator. The 512 at
// n = 128 wraps to a stored 0: the normaliser gives that case one shift
// less (512 = 256 << 1), so the reciprocal is always 256 + f.
static uint8_t recipf_[128];

// Normalising tables, indexed by a byte b >= 1 with lz(b) its leading zero
// bits (entry 0 unused):
//   nrm_[b]   = b << lz(b), the byte shifted up until bit 7 is set
//   k8_[b]    = 8 - lz(b), the byte's bit length (1..8)
//   sbyte_[b] = e = s + 8 for a byte denominator b: 8 - lz(b), one less when b
//               is a power of two (normalised to 128, whose stored f of 0
//               then means 256 at one shift less - 2^16/128 = 256 << 1 - so
//               no multiply needs a special case)
static uint8_t nrm_[256];
static uint8_t k8_[256];
static uint8_t sbyte_[256];

// sin of [0, pi) in u0.8, 128 entries (256ths of a turn): every entry is
// positive and a stored 0 means 256. The second half turn reuses the table
// negated, and indices 0 and 128 are special cased to 0 without a lookup.
// Taken from sinq_ for pt_rot and its kin, which work in 256ths.
static uint8_t sin_[128];

// The packed-delta tables, all bytes (see "packed delta tables" below):
//   sinq_[j]  round(256 sin(j/256 of a right angle)), j = 0..255; from
//             PT_SIN_ONE on it's 256, never read (the index is compared)
//   atan_[i]  floor(atan((i + 0.5) / 256)) in 1024ths of a turn, 0..127: the
//             angle of a ratio byte, at the middle of its 1/256
//   lhyp_[i]  round(256 log2 sqrt(1 + ((i + 0.5) / 256)^2)): log2 of the
//             length over the major component, for the same ratio byte
//   logf_[j]  round(256 log2(1 + j / 256)): log2's fraction for a mantissa
//   expf_[f]  round(256 (2^(f / 256) - 1)): 2^x's mantissa for a fraction
//   lsec_[i]  round(-256 log2 cos((i + 0.5) / 1024 of a turn)), i = 0..127:
//             from a length to a depth, at the middle of an angle's step
//   a2c_[k]   round(64 + 64 tan((k - 128 + 0.5) / 1024 of a turn)), 0..128:
//             the column of the middle of view angle k - 128
static uint8_t sinq_[256];
static uint8_t atan_[256];
static uint8_t lhyp_[256];
static uint8_t logf_[256];
static uint8_t expf_[256];
static uint8_t lsec_[128];
static uint8_t a2c_[256];

static bool inited_;

// ---- packed delta tables ----
//
// The 6502 builds a table of a smooth function from its first entry and a
// bit for each delta after it, over a base for each byte of bits (0, 1 or
// 2, from comparing the byte index on the 6502: a run of bases per table).
// The bits come out by ROR through the carry. The first byte holds fewer
// than 8, with a set bit just above them; that bit, carried round into
// each next byte, sits above its 8, so a byte has run out when ROR leaves
// it zero. The bytes are stored last first, so X counts down:
//
//       LDA #first : LDY #0 : LDX #nbytes - 1 : CLC : BCC start
//   .loop
//       ADC #0                      ; the delta's bit
//       CPX #run : ADC #0           ; the byte's base, as runs need
//   .start
//       STA table,Y : INY
//   .next
//       ROR bits,X : BNE loop
//       DEX : BPL next              ; the set bit, in the carry, goes on
//
// The last add of an entry must leave the carry clear, as ROR shifts it
// in: the sine's wrap from 255 to 256 (a stored 0) comes on its delta's
// add, and the base's compare clears it. The bits here come from the
// ideal values, as Baron's FOR loops would make them at assembly time,
// and unpack is the loop above.

typedef struct packed {
    uint8_t bits[32];
    uint8_t base[32];
    uint8_t first;    // the first entry
} packed;

// The deltas of n ideal values (which may run past 255: the table stores
// them modulo 256), a bit each over their byte's base. A byte whose deltas
// fit its predecessor's base keeps it, so the bases run monotonically.
static packed pack(const int32_t *ideal, uint32_t n)
{
    packed p = {.first = (uint8_t)ideal[0]};
    uint32_t k = (n - 1) % 8;    // the first byte's share
    uint32_t nbytes = (n - 1) / 8 + 1;
    RC_ASSERT(nbytes <= 32);
    uint32_t d = 0;
    for (uint32_t b = 0; b < nbytes; b++) {
        uint32_t cnt = b == 0 ? k : 8;
        int32_t lo = 255, hi = 0;
        for (uint32_t j = 0; j < cnt; j++) {
            int32_t dj = ideal[d + j + 1] - ideal[d + j];
            lo = dj < lo ? dj : lo;
            hi = dj > hi ? dj : hi;
        }
        int32_t base = cnt == 0 ? 0 : lo;
        bool prev = b > 1 || (b == 1 && k > 0);    // the byte before holds deltas
        if (prev && p.base[b - 1] <= lo && hi <= p.base[b - 1] + 1)
            base = p.base[b - 1];
        RC_PANIC(cnt == 0 || (base >= 0 && hi - base <= 1));
        p.base[b] = (uint8_t)base;
        uint8_t bits = b == 0 ? (uint8_t)(1u << k) : 0;    // the first byte's set bit
        for (uint32_t j = 0; j < cnt; j++)
            if (ideal[d + j + 1] - ideal[d + j] > base)
                bits |= (uint8_t)(1u << j);
        p.bits[b] = bits;
        d += cnt;
    }
    return p;
}

// The 6502's loop: ROR each byte through the carry until it's empty.
static void unpack(uint8_t *table, uint32_t n, packed p)
{
    uint8_t v = p.first;
    uint32_t y = 0;
    table[y++] = v;
    bool c = false;
    for (uint32_t b = 0; y < n; b++) {
        uint8_t bits = p.bits[b];
        for (;;) {
            bool out = bits & 1;    // ROR bits,X
            bits = (uint8_t)(bits >> 1 | (c ? 0x80 : 0));
            c = out;
            if (bits == 0)
                break;    // c holds the set bit for the next byte
            v = (uint8_t)(v + c + p.base[b]);    // ADC #0, then the base
            c = false;
            table[y++] = v;
        }
    }
    RC_ASSERT(y == n);
}

static void build(uint8_t *table, uint32_t n, double (*f)(uint32_t), bool floored)
{
    int32_t ideal[256];
    for (uint32_t i = 0; i < n; i++)
        ideal[i] = (int32_t)(floored ? floor(f(i)) : floor(f(i) + 0.5));
    unpack(table, n, pack(ideal, n));
}

#define PI 3.14159265358979323846

static double f_sinq(uint32_t j) { return 256.0 * sin(j * (PI / 512.0)); }
static double f_atan(uint32_t i) { return atan((i + 0.5) / 256.0) * (512.0 / PI); }
static double f_lhyp(uint32_t i) { return 128.0 * log2(1.0 + ((i + 0.5) / 256.0) * ((i + 0.5) / 256.0)); }
static double f_logf(uint32_t j) { return 256.0 * log2(1.0 + j / 256.0); }
static double f_expf(uint32_t f) { return 256.0 * (exp2(f / 256.0) - 1.0); }
static double f_lsec(uint32_t i) { return -256.0 * log2(cos((i + 0.5) * (PI / 512.0))); }
static double f_a2c(uint32_t k) { return 64.0 + 64.0 * tan(((double)k - 128 + 0.5) * (PI / 512.0)); }

void pt_arith_init(void)
{
    if (inited_)
        return;

    // f(x + 1) = f(x) + floor((x + 1) / 2) for f(x) = x*x/4 floored; sq2 is
    // sq1 mirrored about 255
    sq1_[0] = 0;
    for (uint32_t i = 0; i < 511; i++)
        sq1_[i + 1] = (uint16_t)(sq1_[i] + (i + 1) / 2);
    for (uint32_t i = 0; i < 512; i++)
        sq2_[i] = sq1_[i >= 255 ? i - 255 : 255 - i];

    build(sinq_, 256, f_sinq, false);
    build(atan_, 256, f_atan, true);
    build(lhyp_, 256, f_lhyp, false);
    build(logf_, 256, f_logf, false);
    build(expf_, 256, f_expf, false);
    build(lsec_, 128, f_lsec, false);
    build(a2c_, 256, f_a2c, false);

    for (uint32_t n = 128; n < 256; n++)
        recipf_[n - 128] = (uint8_t)((0x10000u + n / 2) / n - 256);

    for (uint32_t b = 1; b < 256; b++) {
        uint32_t lz = 0;
        while ((b << lz) < 128)
            lz++;
        nrm_[b] = (uint8_t)(b << lz);
        k8_[b] = (uint8_t)(8 - lz);
        sbyte_[b] = (uint8_t)(8 - lz - (nrm_[b] == 128));
    }

    // a 256th of a turn is four 1024ths; past the right angle, the mirror
    sin_[0] = 0;    // never read: index 0 is special cased
    for (uint32_t i = 1; i < 128; i++) {
        uint32_t j = 4 * i;
        sin_[i] = j < 256 ? sinq_[j] : j == 256 ? 0 : sinq_[512 - j];    // a stored 0 means 256
    }

    inited_ = true;
}

// ---- angles and logs ----

uint16_t pt_sinq(uint8_t j)
{
    RC_ASSERT(inited_);
    return j >= PT_SIN_ONE ? 256 : sinq_[j];
}

uint16_t pt_log2(uint16_t x)
{
    RC_ASSERT(inited_);
    RC_ASSERT(x > 0);

    // the leading one's place is the whole part, and the 8 bits under it,
    // rounded (the last shift's carry on the 6502), index the fraction; a
    // rounding that carries out is the next octave's fraction 0. The 6502
    // finds the place and lines the bits up with the k8 and nrm tables
    uint8_t e = 15;
    uint16_t m = x;
    while (!(m & 0x8000)) {
        m = (uint16_t)(m << 1);
        e--;
    }
    uint16_t j = (uint16_t)(((m >> 6) + 1) >> 1);    // 256 + the fraction's byte, or 512
    if (j == 512)
        return (uint16_t)((e + 1) << 8);
    return (uint16_t)(e << 8 | logf_[j - 256]);
}

uint16_t pt_exp2(uint16_t l)
{
    RC_ASSERT(inited_);

    // 1.f in 9 bits, shifted by the whole part less 8: left exactly, or
    // right with the last shift's carry rounding
    uint8_t n = (uint8_t)(l >> 8);
    uint16_t m = (uint16_t)(256 + expf_[l & 0xFF]);
    if (n >= 16)
        return 0xFFFF;
    if (n >= 8)
        return (uint16_t)(m << (n - 8));
    return (uint16_t)(((m >> (7 - n)) + 1) >> 1);
}

uint8_t pt_quadrant(int16_t dx, int16_t dy)
{
    // the signs, as LDA dy+1 : ASL A : EOR dx+1 : ROL A : PHP : ROL A :
    // AND #3 has them (bit 1 dy's, bit 0 dx's EOR dy's), except that a zero
    // counts as negative where that puts an axis at its quadrant's start: dx
    // when dy is positive, dy when dx is negative (a zero test each)
    bool nx = dx < 0 || (dx == 0 && dy > 0);
    bool ny = dy < 0 || (dy == 0 && dx < 0);
    return (uint8_t)(ny << 1 | (nx != ny));
}

pt_polar pt_polar_of(int16_t dx, int16_t dy)
{
    RC_ASSERT(inited_);
    RC_ASSERT((dx != 0 || dy != 0) && dx != INT16_MIN && dy != INT16_MIN);

    uint8_t q = pt_quadrant(dx, dy);
    bool odd = q & 1;
    uint16_t ux = (uint16_t)(dx < 0 ? -dx : dx);
    uint16_t uy = (uint16_t)(dy < 0 ? -dy : dy);

    // the minor component over the major as a byte, from the logs'
    // difference: 2^8 * minor / major, 256 meaning equal. The major's log
    // is the length's too, less a factor
    bool steep = uy > ux;
    uint16_t maj = steep ? uy : ux;
    uint16_t mnr = steep ? ux : uy;
    uint16_t lmaj = pt_log2(maj);
    uint16_t r = 0;
    if (mnr != 0) {
        int32_t l = 8 * 256 + pt_log2(mnr) - lmaj;
        r = l < 0 ? 0 : pt_exp2((uint16_t)l);
    }
    uint8_t phi = r == 256 ? 128 : atan_[r];
    uint8_t lh = r == 256 ? 128 : lhyp_[r];

    // phi runs from the major axis: within the quadrant, from its first
    // axis or its second, as steep and odd differ. 255 - phi rather than
    // 256 - phi: atan_ floors, so both octants hold the step the angle is in
    uint8_t a = (steep != odd) ? (uint8_t)~phi : phi;
    return (pt_polar) {.angle = (uint16_t)(q << 8 | a), .lr = (uint16_t)(lmaj + lh)};
}

uint8_t pt_lsec(int8_t v)
{
    RC_ASSERT(inited_);
    // a negative step's middle is |v| - 0.5 from straight ahead: ~v
    return lsec_[v < 0 ? (uint8_t)~v : (uint8_t)v];
}

uint8_t pt_view_col(int8_t v)
{
    RC_ASSERT(inited_);
    return a2c_[(uint8_t)v ^ 0x80];
}

// ---- directions ----

static uint16_t mulu16_frac_round(uint16_t a, uint8_t f);

// The sine of a 10-bit angle at scale 256, as a magnitude (0..256) and a
// sign. The quadrant picks the index, j or (where the sine runs down from
// 1, in quadrants 1 and 3) 256 - j, and a sign for the lower half turn.
// The indices that are 1.0 are compared, never looked up.
static uint16_t sin_mag(uint16_t a, bool *neg)
{
    uint8_t q = (uint8_t)((a >> 8) & 3);
    uint8_t j = (uint8_t)a;
    *neg = q >= 2;
    if (q & 1) {
        if (j < 256 - PT_SIN_ONE + 1)
            return 256;
        return sinq_[(uint8_t)(~j + 1)];    // (j EOR $FF) + 1 = 256 - j
    }
    return j >= PT_SIN_ONE ? 256 : sinq_[j];
}

int16_t pt_mul_sin(int16_t v, uint16_t a)
{
    RC_ASSERT(inited_);
    RC_ASSERT(v != INT16_MIN);
    bool neg;
    uint16_t s = sin_mag(a, &neg);
    uint16_t uv = (uint16_t)(v < 0 ? -v : v);
    uint16_t p = s == 256 ? uv : s == 0 ? 0 : mulu16_frac_round(uv, (uint8_t)s);
    return (v < 0) != neg ? (int16_t)-p : (int16_t)p;
}

int16_t pt_mul_cos(int16_t v, uint16_t a)
{
    return pt_mul_sin(v, (uint16_t)(a + 256));
}

int16_t pt_dot_dir(int16_t ux, int16_t uy, uint16_t a)
{
    return (int16_t)(pt_mul_cos(ux, a) + pt_mul_sin(uy, a));
}

pt_xy pt_dir(int16_t mag, uint16_t a)
{
    return (pt_xy) {.x = pt_mul_cos(mag, a), .y = pt_mul_sin(mag, a)};
}

uint16_t pt_mulu8(uint8_t a, uint8_t b)
{
    RC_ASSERT(inited_);
    return (uint16_t)(sq1_[(uint32_t)a + b] - sq2_[(uint32_t)a - b + 255]);
}

uint16_t pt_mulu8_neg(uint8_t a, uint8_t b)
{
    RC_ASSERT(inited_);
    return (uint16_t)(sq2_[(uint32_t)a - b + 255] - sq1_[(uint32_t)a + b]);
}

uint16_t pt_mulu16_frac(uint16_t a, uint8_t f)
{
    // (al*f)>>8 discards only bits the true >>8 also discards, so this is
    // exact, and the sum cannot carry out of 16 bits
    uint16_t lo = (uint16_t)(pt_mulu8((uint8_t)a, f) >> 8);
    uint16_t hi = pt_mulu8((uint8_t)(a >> 8), f);
    return (uint16_t)(lo + hi);
}

uint16_t pt_mulu16_frac_neg(uint16_t a, uint8_t f)
{
    // the 6502 build accumulates the pt_mulu8_neg partials directly; the
    // effect is the two's complement of the positive product
    return (uint16_t)-pt_mulu16_frac(a, f);
}

uint16_t pt_mulg_u16(uint16_t g, uint8_t dx)
{
    uint16_t hi = pt_mulu8((uint8_t)(g >> 8), dx);
    if (hi > 0xFF)
        return 0xFFFF;
    uint16_t base = (uint16_t)(hi << 8);
    uint16_t s = (uint16_t)(base + pt_mulu8((uint8_t)g, dx));
    return s < base ? 0xFFFF : s;    // carry out: saturate
}

// As pt_mulu16_frac, but rounded to nearest: add 0x80 to the low partial
// before its shift (one add-with-carry on the 6502).
static uint16_t mulu16_frac_round(uint16_t a, uint8_t f)
{
    uint16_t lo = (uint16_t)((uint16_t)(pt_mulu8((uint8_t)a, f) + 128) >> 8);
    uint16_t hi = pt_mulu8((uint8_t)(a >> 8), f);
    return (uint16_t)(lo + hi);
}

// One rotation term: the unsigned rounded product of a magnitude and the
// sine at table index i (index 0 is zero, a stored 0 means 256).
static uint16_t rot_term(uint16_t ua, uint8_t i, uint8_t v)
{
    if (i == 0)
        return 0;
    if (v == 0)
        return ua;
    return mulu16_frac_round(ua, v);
}

// pa with sign na plus pb with sign nb, in two's complement: opposite signs
// are one subtraction whose wraparound is the answer; only two negative
// terms negate (or accumulate from the negated-product table on the 6502).
static int16_t acc2(uint16_t pa, bool na, uint16_t pb, bool nb)
{
    if (na != nb)
        return na ? (int16_t)(pb - pa) : (int16_t)(pa - pb);
    uint16_t s = (uint16_t)(pa + pb);
    return na ? (int16_t)-s : (int16_t)s;
}

pt_xy pt_rot(int16_t x, int16_t y, uint8_t angle)
{
    RC_ASSERT(inited_);
    RC_ASSERT(x != INT16_MIN && y != INT16_MIN);

    uint8_t si = (uint8_t)(angle & 0x7F);
    uint8_t sv = sin_[si];
    bool sneg = (angle & 0x80) != 0;
    uint8_t ca = (uint8_t)(angle + 64);
    uint8_t ci = (uint8_t)(ca & 0x7F);
    uint8_t cv = sin_[ci];
    bool cneg = (ca & 0x80) != 0;

    // the magnitude/sign splits and the four products are shared between the
    // two components. Rounded to nearest, not truncated: each component sums
    // two products, and a shared truncation bias would lean every near-clip
    // interpolation the same way (visible where a floor line grazes the
    // screen edge).
    uint16_t ux = (uint16_t)(x < 0 ? -x : x);
    uint16_t uy = (uint16_t)(y < 0 ? -y : y);
    bool nx = x < 0;
    bool ny = y < 0;
    return (pt_xy) {
        .x = acc2(rot_term(ux, ci, cv), nx != cneg, rot_term(uy, si, sv), ny != sneg),
        .y = acc2(rot_term(uy, ci, cv), ny != cneg, rot_term(ux, si, sv), nx == sneg),
    };
}

// The signed sine of an angle at scale 256, decoded exactly as pt_rot's
// terms decode it: index 0 is zero, a stored 0 means 256, bit 7 the sign.
static int16_t sin256(uint8_t angle)
{
    uint8_t i = (uint8_t)(angle & 0x7F);
    if (i == 0)
        return 0;
    int16_t v = sin_[i] == 0 ? 256 : (int16_t)sin_[i];
    return (angle & 0x80) ? (int16_t)-v : v;
}

void pt_ray_basis(uint8_t angle, int16_t *k1, int16_t *k2)
{
    RC_ASSERT(inited_);
    int16_t s = sin256(angle);
    int16_t c = sin256((uint8_t)(angle + 64));
    *k1 = (int16_t)(c - s);
    *k2 = (int16_t)(c + s);
}

int16_t pt_lerp16(int16_t a, int16_t b, uint8_t t)
{
    if (b >= a)
        return (int16_t)(a + (int16_t)pt_mulu16_frac((uint16_t)(b - a), t));
    return (int16_t)(a - (int16_t)pt_mulu16_frac((uint16_t)(a - b), t));
}

pt_recip pt_recip8(uint8_t n)
{
    RC_ASSERT(inited_);
    RC_ASSERT(n > 0);

    // pure lookups: left shifts are exact
    return (pt_recip) {.f = recipf_[nrm_[n] - 128], .e = sbyte_[n]};
}

pt_recip pt_recip16(uint16_t n)
{
    RC_ASSERT(n > 0);

    if (n < 256)
        return pt_recip8((uint8_t)n);

    // a u16 shifts right by the high byte's bit length: the normalised byte
    // is the high byte's bits moved up, ORed with the low byte shifted down
    // (its low bits fall away: truncating, costing up to n/2^s in the
    // denominator) - on the 6502 a shift of one byte in A, not of two
    uint8_t hi = (uint8_t)(n >> 8);
    uint8_t s = k8_[hi];
    uint8_t m = (uint8_t)(nrm_[hi] | ((uint8_t)n >> s));
    return (pt_recip) {.f = recipf_[m - 128], .e = (uint8_t)(s + 8 - (m == 128))};
}

// A reciprocal product as the 6502 holds it: 16 bits, with the 17th (the
// final add's carry) beside them. Each caller knows it's clear (its asserts
// keep the product small), takes it as saturation, or shifts it back in.
typedef struct prod17 {
    uint16_t v;
    bool c;
} prod17;

// num * (256 + f) for a byte numerator: one lookup with num added into the
// high byte, exact (the 17th bit is the add's carry).
static prod17 mul_recip8(uint8_t num, pt_recip rc)
{
    uint16_t p = pt_mulu8(num, rc.f);
    uint16_t hi = (uint16_t)((p >> 8) + num);
    return (prod17) {.v = (uint16_t)(hi << 8 | (p & 0xFF)), .c = hi > 0xFF};
}

// num * (256 + f) >> 8 for a u16 numerator: num plus the fractional product
// (two lookups), the add's carry the 17th bit. Exact: only the low partial
// has bits below the shift, and they are the true product's own.
static prod17 mul_recip16(uint16_t num, pt_recip rc)
{
    uint16_t v = (uint16_t)(num + pt_mulu16_frac(num, rc.f));
    return (prod17) {.v = v, .c = v < num};
}

// p >> n for n >= 1: the 17th bit comes in on the first shift, and the rest
// are 16-bit.
static uint16_t shr17(prod17 p, uint8_t n)
{
    RC_ASSERT(n >= 1 && n <= 16);
    uint16_t v = (uint16_t)((p.c ? 0x8000u : 0) | p.v >> 1);
    return (uint16_t)(v >> (n - 1));
}

uint8_t pt_div_t(uint16_t num, uint16_t den)
{
    RC_ASSERT(den > 0 && num <= den);

    // k = 8: a byte den leaves num a byte too, and the exact product shifts
    // by e; a u16 den (e >= 8) takes the u16 form, shifted by e - 8. Either
    // way the shift is 8 at most, so a 17th bit means t > 255
    prod17 p;
    uint8_t s;
    if (den < 256) {
        pt_recip rc = pt_recip8((uint8_t)den);
        p = mul_recip8((uint8_t)num, rc);
        s = rc.e;
    } else {
        pt_recip rc = pt_recip16(den);
        p = mul_recip16(num, rc);
        s = (uint8_t)(rc.e - 8);
    }
    RC_ASSERT(s <= 8);
    if (p.c)
        return 255;
    uint16_t t = (uint16_t)(p.v >> s);
    return t > 255 ? 255 : (uint8_t)t;
}

uint8_t pt_div_cols(uint16_t num_q8, uint16_t den_q8)
{
    RC_ASSERT(den_q8 > 0);

    // k = 0: the u16 form shifted by e; e is 0 only for den_q8 = 1, whose f
    // of 0 leaves no 17th bit
    pt_recip rc = pt_recip16(den_q8);
    prod17 m = mul_recip16(num_q8, rc);
    RC_ASSERT(rc.e > 0 || !m.c);
    uint16_t q = rc.e == 0 ? m.v : shr17(m, rc.e);
    return q > 127 ? 127 : (uint8_t)q;
}

uint16_t pt_div_q8(uint16_t num, uint16_t den)
{
    RC_ASSERT(den >= 128);

    // an overflowing quotient saturates up front (only a byte den can
    // overflow: one compare of num's high byte); then k = 8 is the u16 form
    // shifted right by e - 8, where a shift of 0 (den 129..257) saturates on
    // a 17th bit - or left by one for den = 128 exactly, the one power of two
    // here whose e is 7
    if (den < 256 && (num >> 8) >= den)
        return 0xFFFF;
    pt_recip rc = pt_recip16(den);
    prod17 m = mul_recip16(num, rc);
    if (rc.e < 8)
        return (m.c || (m.v & 0x8000)) ? 0xFFFF : (uint16_t)(m.v << 1);
    if (rc.e == 8)
        return m.c ? 0xFFFF : m.v;
    return shr17(m, (uint8_t)(rc.e - 8));
}

uint16_t pt_grad_mag(uint8_t dy, uint8_t dx)
{
    RC_ASSERT(dx >= 1);
    RC_ASSERT(dy <= 64);

    // k = 8: the exact product shifted by e (0..8: dx = 1 is f = 0, e = 0,
    // giving dy << 8 with no special case); dy <= 64 keeps it under 2^15
    pt_recip rc = pt_recip8(dx);
    prod17 p = mul_recip8(dy, rc);
    RC_ASSERT(!p.c);
    uint16_t q = (uint16_t)(p.v >> rc.e);
    return q > 0x7FFF ? 0x7FFF : q;
}

uint16_t pt_gradq8_mag(uint8_t dy, uint16_t dx_q8)
{
    RC_ASSERT(dx_q8 > 0);
    RC_ASSERT(dy <= 64);

    // k = 16: the exact product (under 2^15: dy <= 64) shifted by e - 8
    // either way; a left shift saturates, tested against the product before
    // shifting
    pt_recip rc = pt_recip16(dx_q8);
    prod17 p = mul_recip8(dy, rc);
    RC_ASSERT(!p.c);
    if (rc.e < 8) {
        if (p.v >= (0x8000u >> (8 - rc.e)))
            return 0x7FFF;
        return (uint16_t)(p.v << (8 - rc.e));
    }
    uint16_t q = (uint16_t)(p.v >> (rc.e - 8));
    return q > 0x7FFF ? 0x7FFF : q;
}

uint8_t pt_project_col(int16_t y, uint16_t x, pt_recip rx)
{
    RC_ASSERT(x > 0);

    uint16_t uy = (uint16_t)(y < 0 ? -y : y);
    RC_ASSERT(uy <= x);

    // k = 6: up to x = 32 (e < 6) uy is a byte and the exact product
    // shifts by e + 2; otherwise the u16 form shifts by e - 6. Neither has a
    // 17th bit: the byte form is under 2^14, and the u16 form under 2 * uy,
    // where uy <= 32768
    prod17 m = rx.e < 6 ? mul_recip8((uint8_t)uy, rx) : mul_recip16(uy, rx);
    RC_ASSERT(!m.c);
    uint16_t q = (uint16_t)(rx.e < 6 ? m.v >> (rx.e + 2) : m.v >> (rx.e - 6));
    if (q > 64)
        q = 64;
    return (uint8_t)(y < 0 ? 64 - q : 64 + q);
}

uint16_t pt_project_col_q8(int16_t y, uint16_t d)
{
    RC_ASSERT(d > 0);

    uint16_t uy = (uint16_t)(y < 0 ? -y : y);
    if (uy > d)
        uy = d;    // interpolation rounding can nudge y just past the ray

    // k = 14: the shift e - 14 is negative at every near-clip depth, so the
    // numerator is pre-shifted left (exact); uy <= d keeps it at most
    // 16,511 (d's normaliser truncates, so a little past 2^14), and the
    // product under 2^15
    pt_recip rc = pt_recip16(d);
    RC_ASSERT(rc.e <= 14);
    prod17 m = mul_recip16((uint16_t)(uy << (14 - rc.e)), rc);
    RC_ASSERT(!m.c);
    uint16_t q = m.v;
    if (q > 64u << 8)
        q = 64u << 8;
    return (uint16_t)(y < 0 ? (64u << 8) - q : (64u << 8) + q);
}

uint16_t pt_project_q8(uint16_t len, uint16_t x, pt_recip rx)
{
    RC_ASSERT(x > 0);

    // k = 14: a byte len takes the exact product, shifted by e - 6; a u16
    // len the u16 form, shifted by e - 14. A 17th bit comes in on a right
    // shift and saturates otherwise, as does a left shift past 16 bits;
    // both shift left at small depths, where the result saturates soonest
    uint8_t k = 6;
    prod17 p;
    if (len < 256) {
        p = mul_recip8((uint8_t)len, rx);
    } else {
        p = mul_recip16(len, rx);
        k = 14;
    }
    if (rx.e > k)
        return shr17(p, (uint8_t)(rx.e - k));
    if (p.c)
        return 0xFFFF;
    uint8_t l = (uint8_t)(k - rx.e);
    return p.v > (0xFFFFu >> l) ? 0xFFFF : (uint16_t)(p.v << l);
}

uint8_t pt_project_rowd(uint8_t h8, uint16_t x, pt_recip rx)
{
    RC_ASSERT(x > 0);
    RC_ASSERT((uint32_t)x * PT_SCREEN_HEIGHT >= (uint32_t)h8 * (8 * 128));    // on screen

    // k = 9 (64 * 8 * h8 / x): the exact product shifted by e - 1, the 17th
    // bit coming in on the shift. e is 0 or 1 only at depths 1 and 2, where
    // h8 can only be 0
    prod17 p = mul_recip8(h8, rx);
    RC_ASSERT(rx.e >= 2 || !p.c);
    uint16_t q = rx.e < 2 ? (uint16_t)(p.v << (1 - rx.e)) : shr17(p, (uint8_t)(rx.e - 1));
    if (q > PT_SCREEN_HEIGHT / 2)
        q = PT_SCREEN_HEIGHT / 2;
    return (uint8_t)q;
}
