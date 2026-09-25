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

// recip_[n] = 0x10000 / n; entries 0 and 1 are saturated.
static uint16_t recip_[256];

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

    recip_[0] = 0xFFFF;
    recip_[1] = 0xFFFF;
    for (uint32_t n = 2; n < 256; n++)
        recip_[n] = (uint16_t)(0x10000u / n);

    sin_[0] = 0;    // never read: index 0 is special cased
    for (uint32_t i = 1; i < 128; i++) {
        long v = lround(sin((double)i * (3.14159265358979323846 / 128.0)) * 256.0);
        sin_[i] = v >= 256 ? 0 : (uint8_t)v;    // a stored 0 means 256
    }

    inited_ = true;
}

int16_t pt_sin(uint8_t angle)
{
    RC_ASSERT(inited_);

    uint8_t i = (uint8_t)(angle & 0x7F);
    if (i == 0)
        return 0;    // angles 0 and 128
    uint8_t v = sin_[i];
    int16_t mag = v == 0 ? 256 : v;
    return (angle & 0x80) ? (int16_t)-mag : mag;
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

int16_t pt_muls16_sine(int16_t a, int16_t s)
{
    RC_ASSERT(s >= -256 && s <= 256);
    RC_ASSERT(a != INT16_MIN);

    if (s == 256)
        return a;
    if (s == -256)
        return (int16_t)-a;

    // rounded to nearest, not truncated: the transform sums two of these per
    // coordinate, and a shared truncation bias would lean every near-clip
    // interpolation the same way (visible where a floor line grazes the
    // screen edge)
    uint16_t ua = (uint16_t)(a < 0 ? -a : a);
    uint8_t f = (uint8_t)(s < 0 ? -s : s);
    uint16_t m = mulu16_frac_round(ua, f);
    if ((a < 0) == (s < 0))
        return (int16_t)m;
    return (int16_t)-m;
}

int16_t pt_lerp16(int16_t a, int16_t b, uint8_t t)
{
    if (b >= a)
        return (int16_t)(a + (int16_t)pt_mulu16_frac((uint16_t)(b - a), t));
    return (int16_t)(a - (int16_t)pt_mulu16_frac((uint16_t)(a - b), t));
}

// The one deliberately wide helper: a full 16x16 product, which on the 6502
// is the four-partial byte chain of the quarter-square multiply; each caller
// shifts one window out of it and the result widths below all fit u16.
static uint32_t mulwide(uint16_t a, uint16_t b)
{
    uint32_t p = pt_mulu8((uint8_t)a, (uint8_t)b);
    p += (uint32_t)pt_mulu8((uint8_t)a, (uint8_t)(b >> 8)) << 8;
    p += (uint32_t)pt_mulu8((uint8_t)(a >> 8), (uint8_t)b) << 8;
    p += (uint32_t)pt_mulu8((uint8_t)(a >> 8), (uint8_t)(b >> 8)) << 16;
    return p;
}

pt_recip pt_recip16(uint16_t n)
{
    RC_ASSERT(inited_);
    RC_ASSERT(n > 0);

    uint8_t s = 0;
    while (n > 255) {
        n >>= 1;
        s++;
    }
    return (pt_recip) {.r = recip_[n], .s = s};
}

uint8_t pt_div_t(uint16_t num, uint16_t den)
{
    RC_ASSERT(den > 0 && num <= den);

    pt_recip rc = pt_recip16(den);
    uint32_t t = mulwide(num, rc.r) >> (8 + rc.s);
    return t > 255 ? 255 : (uint8_t)t;
}

uint8_t pt_div_cols(uint16_t num_q8, uint16_t den_q8)
{
    RC_ASSERT(den_q8 > 0);

    pt_recip rc = pt_recip16(den_q8);
    uint32_t q = mulwide(num_q8, rc.r) >> (16 + rc.s);
    return q > 127 ? 127 : (uint8_t)q;
}

uint16_t pt_div_q8(uint32_t num, uint16_t den)
{
    RC_ASSERT(den > 0);
    RC_ASSERT(num < (1u << 24));

    // (num * r) >> (8 + s), the numerator split into high and low bytes so
    // each partial fits the u16 x u16 chain
    pt_recip rc = pt_recip16(den);
    uint32_t q = (mulwide((uint16_t)(num >> 8), rc.r) >> rc.s)
               + (mulwide((uint16_t)(num & 0xFF), rc.r) >> (8 + rc.s));
    return q > 0xFFFF ? 0xFFFF : (uint16_t)q;
}

uint16_t pt_grad_mag(uint8_t dy, uint8_t dx)
{
    RC_ASSERT(dx >= 1);
    RC_ASSERT(dy <= 64);

    if (dx == 1)
        return (uint16_t)(dy << 8);
    uint32_t q = mulwide(dy, recip_[dx]) >> 8;
    return q > 0x7FFF ? 0x7FFF : (uint16_t)q;
}

uint16_t pt_gradq8_mag(uint8_t dy, uint16_t dx_q8)
{
    RC_ASSERT(dx_q8 > 0);
    RC_ASSERT(dy <= 64);

    pt_recip rc = pt_recip16(dx_q8);
    uint32_t q = mulwide(dy, rc.r) >> rc.s;
    return q > 0x7FFF ? 0x7FFF : (uint16_t)q;
}

uint8_t pt_project_col(int16_t y, uint16_t x)
{
    RC_ASSERT(x > 0);

    uint16_t uy = (uint16_t)(y < 0 ? -y : y);
    RC_ASSERT(uy <= x);

    pt_recip rc = pt_recip16(x);
    uint32_t q = mulwide(uy, rc.r) >> (10 + rc.s);
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

    pt_recip rc = pt_recip16(d);
    uint32_t q = mulwide(uy, rc.r) >> (2 + rc.s);
    if (q > 64u << 8)
        q = 64u << 8;
    return (uint16_t)(y < 0 ? (64u << 8) - q : (64u << 8) + q);
}

uint8_t pt_project_row(int16_t h, uint16_t x)
{
    RC_ASSERT(x > 0);

    uint16_t uh = (uint16_t)(h < 0 ? -h : h);
    RC_ASSERT((uint32_t)x * PT_SCREEN_HEIGHT >= (uint32_t)uh * 128);    // on screen vertically

    pt_recip rc = pt_recip16(x);
    uint32_t q = mulwide(uh, rc.r) >> (10 + rc.s);
    if (q > PT_SCREEN_HEIGHT / 2)
        q = PT_SCREEN_HEIGHT / 2;
    return (uint8_t)(h < 0 ? PT_SCREEN_HEIGHT / 2 - q : PT_SCREEN_HEIGHT / 2 + q);
}
