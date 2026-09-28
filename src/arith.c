// arith.c - table builders and the arithmetic helpers. The tables are built
// with ordinary C arithmetic (they would be baked into the 6502 binary); the
// helpers themselves stick to lookups, adds, shifts and unsigned compares,
// with widths held to what the 6502 routine would keep.

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

// sin of [0, pi) in u0.8, 128 entries: every entry is positive and a stored
// 0 means 256. The second half turn reuses the table negated, and indices 0
// and 128 are special cased to 0 without a lookup.
static uint8_t sin_[128];

static bool inited_;

void pt_arith_init(void)
{
    if (inited_)
        return;

    for (uint32_t i = 0; i < 512; i++) {
        sq1_[i] = (uint16_t)((i * i) / 4);
        int32_t d = (int32_t)i - 255;
        sq2_[i] = (uint16_t)((d * d) / 4);
    }

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

    sin_[0] = 0;    // never read: index 0 is special cased
    for (uint32_t i = 1; i < 128; i++) {
        long v = lround(sin((double)i * (3.14159265358979323846 / 128.0)) * 256.0);
        sin_[i] = v >= 256 ? 0 : (uint8_t)v;    // a stored 0 means 256
    }

    inited_ = true;
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

// One direction component: mag times the sine at index i, exact (an 8-bit
// product lands on the s8.8 scale directly), with the half turn's sign.
static int16_t dir_term(uint8_t mag, uint8_t i, uint8_t v, bool neg)
{
    if (i == 0)
        return 0;
    uint16_t p = v == 0 ? (uint16_t)((uint16_t)mag << 8) : pt_mulu8(mag, v);
    return neg ? (int16_t)-p : (int16_t)p;
}

pt_xy pt_dir8(uint8_t mag, uint8_t angle)
{
    RC_ASSERT(inited_);
    RC_ASSERT(mag <= 127);

    uint8_t si = (uint8_t)(angle & 0x7F);
    uint8_t ca = (uint8_t)(angle + 64);
    uint8_t ci = (uint8_t)(ca & 0x7F);
    return (pt_xy) {
        .x = dir_term(mag, ci, sin_[ci], (ca & 0x80) != 0),
        .y = dir_term(mag, si, sin_[si], (angle & 0x80) != 0),
    };
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

// num * (256 + f) for a byte numerator: one lookup with num added into the
// high byte, exact in 17 bits (the top bit is the add's carry).
static uint32_t mul_recip8(uint8_t num, pt_recip rc)
{
    return ((uint32_t)num << 8) + pt_mulu8(num, rc.f);
}

// num * (256 + f) >> 8 for a u16 numerator: num plus the fractional product
// (two lookups), 17 bits with the carry. Exact: only the low partial has
// bits below the shift, and they are the true product's own.
static uint32_t mul_recip16(uint16_t num, pt_recip rc)
{
    return (uint32_t)num + pt_mulu16_frac(num, rc.f);
}

uint8_t pt_div_t(uint16_t num, uint16_t den)
{
    RC_ASSERT(den > 0 && num <= den);

    // k = 8: a byte den leaves num a byte too, and the exact product shifts
    // by e; a u16 den (e >= 8) takes the u16 form, shifted by e - 8
    uint32_t t;
    if (den < 256) {
        pt_recip rc = pt_recip8((uint8_t)den);
        t = mul_recip8((uint8_t)num, rc) >> rc.e;
    } else {
        pt_recip rc = pt_recip16(den);
        t = mul_recip16(num, rc) >> (rc.e - 8);
    }
    return t > 255 ? 255 : (uint8_t)t;
}

uint8_t pt_div_cols(uint16_t num_q8, uint16_t den_q8)
{
    RC_ASSERT(den_q8 > 0);

    // k = 0: the u16 form shifted by e
    pt_recip rc = pt_recip16(den_q8);
    uint32_t q = mul_recip16(num_q8, rc) >> rc.e;
    return q > 127 ? 127 : (uint8_t)q;
}

uint16_t pt_div_q8(uint16_t num, uint16_t den)
{
    RC_ASSERT(den >= 128);

    // an overflowing quotient saturates up front (only a byte den can
    // overflow: one compare of num's high byte); then k = 8 is the u16 form
    // shifted right by e - 8 - or left by one for den = 128 exactly, the one
    // power of two here whose e is 7
    if (den < 256 && (num >> 8) >= den)
        return 0xFFFF;
    pt_recip rc = pt_recip16(den);
    uint32_t m = mul_recip16(num, rc);
    uint32_t q = rc.e < 8 ? m << 1 : m >> (rc.e - 8);
    return q > 0xFFFF ? 0xFFFF : (uint16_t)q;
}

uint16_t pt_grad_mag(uint8_t dy, uint8_t dx)
{
    RC_ASSERT(dx >= 1);
    RC_ASSERT(dy <= 64);

    // k = 8: the exact product shifted by e (0..8: dx = 1 is f = 0, e = 0,
    // giving dy << 8 with no special case)
    pt_recip rc = pt_recip8(dx);
    uint32_t q = mul_recip8(dy, rc) >> rc.e;
    return q > 0x7FFF ? 0x7FFF : (uint16_t)q;
}

uint16_t pt_gradq8_mag(uint8_t dy, uint16_t dx_q8)
{
    RC_ASSERT(dx_q8 > 0);
    RC_ASSERT(dy <= 64);

    // k = 16: the exact product shifted by e - 8 either way; a left shift
    // saturates, tested against the product before shifting
    pt_recip rc = pt_recip16(dx_q8);
    uint32_t p = mul_recip8(dy, rc);
    if (rc.e < 8) {
        if (p >= (0x8000u >> (8 - rc.e)))
            return 0x7FFF;
        return (uint16_t)(p << (8 - rc.e));
    }
    p >>= rc.e - 8;
    return p > 0x7FFF ? 0x7FFF : (uint16_t)p;
}

uint8_t pt_project_col(int16_t y, uint16_t x, pt_recip rx)
{
    RC_ASSERT(x > 0);

    uint16_t uy = (uint16_t)(y < 0 ? -y : y);
    RC_ASSERT(uy <= x);

    // k = 6: below x = 32 (e < 6) uy is a byte and the exact product
    // shifts by e + 2; otherwise the u16 form shifts by e - 6
    uint32_t q = rx.e < 6 ? mul_recip8((uint8_t)uy, rx) >> (rx.e + 2)
                          : mul_recip16(uy, rx) >> (rx.e - 6);
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
    // numerator is pre-shifted left (exact); uy <= d keeps it under
    // n * 64 < 2^14
    pt_recip rc = pt_recip16(d);
    RC_ASSERT(rc.e <= 14);
    uint32_t q = mul_recip16((uint16_t)(uy << (14 - rc.e)), rc);
    if (q > 64u << 8)
        q = 64u << 8;
    return (uint16_t)(y < 0 ? (64u << 8) - q : (64u << 8) + q);
}

uint8_t pt_project_rowd(uint8_t h8, uint16_t x, pt_recip rx)
{
    RC_ASSERT(x > 0);
    RC_ASSERT((uint32_t)x * PT_SCREEN_HEIGHT >= (uint32_t)h8 * (8 * 128));    // on screen

    // k = 9 (64 * 8 * h8 / x): the exact product shifted by e - 1; e is 0
    // only at depth 1, where h8 can only be 0
    uint32_t p = mul_recip8(h8, rx);
    uint32_t q = rx.e == 0 ? p << 1 : p >> (rx.e - 1);
    if (q > PT_SCREEN_HEIGHT / 2)
        q = PT_SCREEN_HEIGHT / 2;
    return (uint8_t)q;
}
