// arith.h - 6502-friendly fixed-point arithmetic: every multiply and divide in
// the engine goes through these helpers, which use only the quarter-square,
// reciprocal and sine tables plus shifts and adds (as the BBC build would).
// Values that are only added or subtracted are ordinary two's complement;
// a multiply or divide works on unsigned magnitudes, with the sign held in a
// condition that picks between two paths (or a negated-product lookup).

#ifndef PT_ARITH_H_
#define PT_ARITH_H_

#include <stdint.h>

// The sign and magnitude of sin(angle), resolved once (per frame for the
// camera direction): m in [0, 256] with the sign kept apart, so a signed
// sine value is never formed.
typedef struct pt_sincos {
    uint16_t m;
    uint8_t neg;
} pt_sincos;

// Build the tables; call once before anything else (idempotent).
void pt_arith_init(void);

// Resolve sin(angle) for 256ths of a turn: the index's low 7 bits pick the
// magnitude from a 128-entry table over [0, pi) (0 and 128 special cased to
// zero, a stored 0 to 256), the top bit is the sign.
pt_sincos pt_sin_resolve(uint8_t angle);

// cos(angle): the sine a quarter turn on.
static inline pt_sincos pt_cos_resolve(uint8_t angle)
{
    return pt_sin_resolve((uint8_t)(angle + 64));
}

// a times a resolved sine, rounded to nearest: the resolved sign and the
// sign of a pick the positive or the negated product path. Rounding keeps
// the vertex transform unbiased.
int16_t pt_mul_sc(int16_t a, pt_sincos sc);

// u*a + v*b for resolved sines: both products stay unsigned and enter the
// sum by add or subtract -- opposite signs are one subtraction whose
// wraparound is the two's complement. Only two negative terms negate at all
// (once, of the finished sum), and the 6502 sidesteps even that by
// accumulating the pair from the negated-product table (pt_mulu8_neg).
int16_t pt_mac_sc(int16_t u, pt_sincos a, int16_t v, pt_sincos b);

// u*a - v*b: the same accumulation with b's sign flipped on the way in.
static inline int16_t pt_msub_sc(int16_t u, pt_sincos a, int16_t v, pt_sincos b)
{
    b.neg ^= 1;
    return pt_mac_sc(u, a, v, b);
}

// a * sin(angle) / a * cos(angle) in one step, for occasional callers.
static inline int16_t pt_mul_sin(int16_t a, uint8_t angle)
{
    return pt_mul_sc(a, pt_sin_resolve(angle));
}

static inline int16_t pt_mul_cos(int16_t a, uint8_t angle)
{
    return pt_mul_sc(a, pt_cos_resolve(angle));
}

// ---- unsigned multiplies ----

// a * b via the quarter-square tables; the primitive everything else builds on.
uint16_t pt_mulu8(uint8_t a, uint8_t b);

// -(a * b) as two's complement: the same two lookups subtracted the other way
// round, so an accumulate can add a negative product without negating.
uint16_t pt_mulu8_neg(uint8_t a, uint8_t b);

// (a * f) >> 8, f a u0.8 fraction; exact, and needs no width beyond u16.
uint16_t pt_mulu16_frac(uint16_t a, uint8_t f);

// -((a * f) >> 8) as two's complement, from the negated-product partials.
uint16_t pt_mulu16_frac_neg(uint16_t a, uint8_t f);

// (v * f) >> 8 for a u24 value: the keep-high byte chain, three lookups
// (fewer when v's top bytes are zero); exact like pt_mulu16_frac.
uint32_t pt_mulu24_frac(uint32_t v, uint8_t f);

// gradient magnitude (u7.8) times a column count, saturating at 0xFFFF.
uint16_t pt_mulg_u16(uint16_t g, uint8_t dx);

// a + t * (b - a) for a u0.8 parameter t: the comparison picks the direction,
// and the difference is multiplied unsigned.
int16_t pt_lerp16(int16_t a, int16_t b, uint8_t t);

// ---- reciprocal-table divisions ----
// Each is num * 2^k / den for its own k: den is normalised, the numerator is
// pre-shifted when the result scale calls for it, and the product is one add
// plus a single u24 x u8 fractional multiply (at most three table lookups,
// fewer when the numerator's top bytes are zero). Error stays within ~0.2%
// of the result plus a count of truncation.

// num / den as a u0.8 clip parameter t, clamped to 255; needs num <= den.
uint8_t pt_div_t(uint16_t num, uint16_t den);

// floor(num / den) for two u8.8 values, clamped to 127: the crossing quotient.
uint8_t pt_div_cols(uint16_t num_q8, uint16_t den_q8);

// (num << 8) / den for a numerator up to 24 bits, clamped to 0xFFFF.
uint16_t pt_div_q8(uint32_t num, uint16_t den);

// (dy << 8) / dx: a gradient magnitude from a row delta and a column count.
uint16_t pt_grad_mag(uint8_t dy, uint8_t dx);

// (dy << 16) / dx_q8: the near clip's fractional-dx gradient magnitude,
// clamped to the u7.8-with-sign-bit range.
uint16_t pt_gradq8_mag(uint8_t dy, uint16_t dx_q8);

// ---- projections ----

// floor(64 + 64*y/x): the screen column of an in-frustum vertex (|y| <= x).
uint8_t pt_project_col(int16_t y, uint16_t x);

// The same projection at depth d kept as u8.8 (the near clip's sx*).
uint16_t pt_project_col_q8(int16_t y, uint16_t d);

// 64*h/x for a height magnitude h = 8*h8 (heights are quantised to 8-unit
// steps so they fit a byte): the row offset of a line endpoint from the
// screen centre, the caller adding or subtracting by the height's side.
// Needs the endpoint on screen vertically, x >= 128*h/PT_SCREEN_HEIGHT (the
// 64-row screen's classic x >= 2h).
uint8_t pt_project_rowd(uint8_t h8, uint16_t x);

#endif
