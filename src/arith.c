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
// n = 128 wraps to a stored 0, which therefore means 256.
static uint8_t recipf_[128];

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

    sin_[0] = 0;    // never read: index 0 is special cased
    for (uint32_t i = 1; i < 128; i++) {
        long v = lround(sin((double)i * (3.14159265358979323846 / 128.0)) * 256.0);
        sin_[i] = v >= 256 ? 0 : (uint8_t)v;    // a stored 0 means 256
    }

    inited_ = true;
}

pt_sincos pt_sin_resolve(uint8_t angle)
{
    RC_ASSERT(inited_);

    uint8_t i = (uint8_t)(angle & 0x7F);
    uint8_t neg = (uint8_t)(angle >> 7);
    if (i == 0)
        return (pt_sincos) {.m = 0, .neg = neg};    // angles 0 and 128
    uint8_t v = sin_[i];
    return (pt_sincos) {.m = v == 0 ? 256 : v, .neg = neg};
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

// One term of a sine multiply or accumulate: the unsigned rounded product
// and the product's sign. Rounded to nearest, not truncated: the transform
// sums two of these per coordinate, and a shared truncation bias would lean
// every near-clip interpolation the same way (visible where a floor line
// grazes the screen edge).
static uint16_t term_sc(int16_t a, pt_sincos sc, bool *neg)
{
    RC_ASSERT(sc.m <= 256);
    RC_ASSERT(a != INT16_MIN);

    uint16_t ua = (uint16_t)(a < 0 ? -a : a);
    *neg = (a < 0) != (sc.neg != 0);
    return sc.m == 256 ? ua : mulu16_frac_round(ua, (uint8_t)sc.m);
}

int16_t pt_mul_sc(int16_t a, pt_sincos sc)
{
    bool neg;
    uint16_t m = term_sc(a, sc, &neg);
    return neg ? (int16_t)-m : (int16_t)m;
}

int16_t pt_mac_sc(int16_t u, pt_sincos a, int16_t v, pt_sincos b)
{
    bool nu;
    bool nv;
    uint16_t pu = term_sc(u, a, &nu);
    uint16_t pv = term_sc(v, b, &nv);
    if (nu != nv)
        return nu ? (int16_t)(pv - pu) : (int16_t)(pu - pv);
    uint16_t s = (uint16_t)(pu + pv);
    return nu ? (int16_t)-s : (int16_t)s;
}

int16_t pt_lerp16(int16_t a, int16_t b, uint8_t t)
{
    if (b >= a)
        return (int16_t)(a + (int16_t)pt_mulu16_frac((uint16_t)(b - a), t));
    return (int16_t)(a - (int16_t)pt_mulu16_frac((uint16_t)(a - b), t));
}

// A normalised reciprocal: 2^16 / den ~= (256 + f) * 2^-s, den shifted into
// [128, 255] (right shifts truncating, left shifts exact, s signed). f is a
// single byte; 0 means 256 (den a power of two), the sine table's trick.
typedef struct pt_recip {
    uint8_t f;
    int8_t s;
} pt_recip;

static pt_recip recip16(uint16_t n)
{
    RC_ASSERT(inited_);
    RC_ASSERT(n > 0);

    int8_t s = 0;
    while (n > 255) {
        n >>= 1;    // truncating; costs up to n/2^s in the denominator
        s++;
    }
    while (n < 128) {
        n <<= 1;    // exact
        s--;
    }
    return (pt_recip) {.f = recipf_[n - 128], .s = s};
}

uint32_t pt_mulu24_frac(uint32_t v, uint8_t f)
{
    // exact: only the first partial has bits below the shift, and they are
    // the true product's own low byte
    uint32_t m = pt_mulu8((uint8_t)v, f) >> 8;
    m += pt_mulu8((uint8_t)(v >> 8), f);
    m += (uint32_t)pt_mulu8((uint8_t)(v >> 16), f) << 8;
    return m;
}

// The division core: num * 2^k / den as num * (256 + f) * 2^-(8 + s - k).
// A net left shift is applied to num first (exact), so the fractional
// truncation is never amplified; what remains is one add, one fractional
// multiply and a right shift.
static uint32_t recip_div(uint32_t num, uint16_t den, uint8_t k)
{
    pt_recip rc = recip16(den);
    int8_t sh = (int8_t)(8 + rc.s - (int8_t)k);
    if (sh < 0) {
        num <<= -sh;
        sh = 0;
    }
    RC_ASSERT(num < (1u << 24));    // every caller's domain keeps the chain u24
    uint32_t m = rc.f == 0 ? num << 1 : num + pt_mulu24_frac(num, rc.f);
    return m >> sh;
}

uint8_t pt_div_t(uint16_t num, uint16_t den)
{
    RC_ASSERT(den > 0 && num <= den);

    uint32_t t = recip_div(num, den, 8);
    return t > 255 ? 255 : (uint8_t)t;
}

uint8_t pt_div_cols(uint16_t num_q8, uint16_t den_q8)
{
    RC_ASSERT(den_q8 > 0);

    uint32_t q = recip_div(num_q8, den_q8, 0);
    return q > 127 ? 127 : (uint8_t)q;
}

uint16_t pt_div_q8(uint32_t num, uint16_t den)
{
    RC_ASSERT(den > 0);
    RC_ASSERT(num < (1u << 24));

    // an overflowing quotient saturates up front; this also caps the
    // pre-shifted numerator within the u24 chain
    if (num >= (uint32_t)den << 8)
        return 0xFFFF;
    uint32_t q = recip_div(num, den, 8);
    return q > 0xFFFF ? 0xFFFF : (uint16_t)q;
}

uint16_t pt_grad_mag(uint8_t dy, uint8_t dx)
{
    RC_ASSERT(dx >= 1);
    RC_ASSERT(dy <= 64);

    if (dx == 1)
        return (uint16_t)(dy << 8);
    uint32_t q = recip_div(dy, dx, 8);
    return q > 0x7FFF ? 0x7FFF : (uint16_t)q;
}

uint16_t pt_gradq8_mag(uint8_t dy, uint16_t dx_q8)
{
    RC_ASSERT(dx_q8 > 0);
    RC_ASSERT(dy <= 64);

    uint32_t q = recip_div(dy, dx_q8, 16);
    return q > 0x7FFF ? 0x7FFF : (uint16_t)q;
}

uint8_t pt_project_col(int16_t y, uint16_t x)
{
    RC_ASSERT(x > 0);

    uint16_t uy = (uint16_t)(y < 0 ? -y : y);
    RC_ASSERT(uy <= x);

    uint32_t q = recip_div(uy, x, 6);
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

    uint32_t q = recip_div(uy, d, 14);
    if (q > 64u << 8)
        q = 64u << 8;
    return (uint16_t)(y < 0 ? (64u << 8) - q : (64u << 8) + q);
}

uint8_t pt_project_rowd(uint8_t h8, uint16_t x)
{
    RC_ASSERT(x > 0);
    RC_ASSERT((uint32_t)x * PT_SCREEN_HEIGHT >= (uint32_t)h8 * (8 * 128));    // on screen

    uint32_t q = recip_div(h8, x, 9);    // 64 * 8 * h8 / x
    if (q > PT_SCREEN_HEIGHT / 2)
        q = PT_SCREEN_HEIGHT / 2;
    return (uint8_t)q;
}
